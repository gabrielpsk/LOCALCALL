"""Janela principal: lista de conectados com Green Ring (VAD), controles de voz/tela, Loopback e E2EE."""
import base64
import os
from pathlib import Path

from PyQt6.QtCore import QPoint, QSize, Qt, QTimer
from PyQt6.QtGui import QAction
from PyQt6.QtWidgets import (QFrame, QHBoxLayout, QLabel, QListWidget,
                             QListWidgetItem, QMainWindow, QMenu, QMessageBox,
                             QPushButton, QSlider, QTabWidget, QVBoxLayout,
                             QWidget, QWidgetAction)

from database.media import MAX_BYTES
from database.store import Store
from network.audio_bridge import AudioBridge, DEFAULT_UDP_PORT
from network.client import ClientSession
from network.host import HostSession
from network.media import run_async
from network.video_bridge import DEFAULT_VIDEO_PORT, MAX_STREAMS, VideoBridge
from .connect_dialog import ConnectDialog
from .i18n import t
from .panels import ChatPanel, NotesPanel, TasksPanel
from .screen_share_dialog import ScreenShareDialog
from .sound_fx import play_chat_ping
from .video_view import VideoStage


class PeerRowWidget(QFrame):
    """Item da lista lateral de participantes com indicador de voz ativa (Green Ring #22c55e) e menu de contexto."""

    def __init__(self, name: str, speaking: bool = False, muted: bool = False, volume: int = 100, on_context_menu=None):
        super().__init__()
        self.peer_name = name
        self.on_context_menu = on_context_menu
        self.setObjectName("peerListRow")
        lay = QHBoxLayout(self)
        lay.setContentsMargins(8, 5, 8, 5)
        lay.setSpacing(6)

        self.dot = QLabel()
        self.dot.setObjectName("voiceRingDot")
        self.dot.setFixedSize(10, 10)

        self.lbl = QLabel(name)
        self.lbl.setObjectName("peerListName")

        self.badge = QLabel()
        self.badge.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)

        lay.addWidget(self.dot)
        lay.addWidget(self.lbl, 1)
        lay.addWidget(self.badge)
        self.set_speaking(speaking)
        self.update_audio_state(muted, volume)

    def update_audio_state(self, muted: bool, volume: int):
        if muted:
            self.badge.setObjectName("peerMuteBadge")
            self.badge.setText(t("peer_badge_muted"))
            self.badge.setVisible(True)
        elif volume != 100:
            self.badge.setObjectName("peerVolBadge")
            self.badge.setText(f"{volume}%")
            self.badge.setVisible(True)
        else:
            self.badge.setText("")
            self.badge.setVisible(False)
        self.badge.style().unpolish(self.badge)
        self.badge.style().polish(self.badge)

    def set_speaking(self, speaking: bool):
        self.setProperty("speaking", speaking)
        self.dot.setProperty("speaking", speaking)
        self.style().unpolish(self)
        self.style().polish(self)
        self.dot.style().unpolish(self.dot)
        self.dot.style().polish(self.dot)

    def contextMenuEvent(self, event):
        if self.on_context_menu and self.peer_name:
            self.on_context_menu(self.peer_name, event.globalPos())
            event.accept()
        else:
            super().contextMenuEvent(event)


class MainWindow(QMainWindow):
    def __init__(self, radmin_notice: str = ""):
        super().__init__()
        self.setWindowTitle("LocalCall")
        self.resize(980, 640)
        self.session = None
        self.store = None
        self.last_name = ""
        self.last_password = ""
        self.radmin_notice = radmin_notice
        self.active_streamers: list[str] = []
        self.sender_id_to_name: dict[int, str] = {}
        self._peer_rows: dict[str, PeerRowWidget] = {}
        self._last_speaking_names: set[str] = set()

        self.audio = AudioBridge()
        self.video = VideoBridge(self)
        self.video.frame_received.connect(self._on_video_frame)

        # Timer de 80 ms para atualizar o Green Ring (Indicador de Voz Ativa)
        self._vad_timer = QTimer(self)
        self._vad_timer.setInterval(80)
        self._vad_timer.timeout.connect(self._poll_speaking)
        self._vad_timer.start()

        # Sidebar
        side = QWidget()
        side.setObjectName("sidebar")
        side.setFixedWidth(210)
        self.peers = QListWidget()
        self.peers.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.peers.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.peers.customContextMenuRequested.connect(self._on_peers_list_context_menu)
        self.conn_button = QPushButton(t("btn_connection"))
        self.conn_button.clicked.connect(self.connect_dialog)
        self.header = QLabel(t("sidebar_connected"))
        self.header.setObjectName("sectionLabel")

        # Controles de áudio e tela no rodapé da barra lateral
        self.voice_header = QLabel(t("sidebar_voice_screen"))
        self.voice_header.setObjectName("sectionLabel")
        self.share_button = QPushButton(t("btn_share_screen"))
        self.share_button.clicked.connect(self.toggle_screen_share)
        self.mute_button = QPushButton(t("btn_mute_mic"))
        self.mute_button.setCheckable(True)
        self.mute_button.toggled.connect(self._on_mute_toggled)
        self.deafen_button = QPushButton(t("btn_deafen"))
        self.deafen_button.setCheckable(True)
        self.deafen_button.toggled.connect(self._on_deafen_toggled)
        self.noise_button = QPushButton(t("btn_noise_on"))
        self.noise_button.setCheckable(True)
        self.noise_button.setChecked(True)
        self.noise_button.setToolTip(t("tooltip_noise_suppression"))
        self.noise_button.toggled.connect(self._on_noise_toggled)

        side_layout = QVBoxLayout(side)
        side_layout.setContentsMargins(10, 12, 10, 10)
        side_layout.setSpacing(6)
        side_layout.addWidget(self.header)
        side_layout.addWidget(self.peers, 1)
        side_layout.addWidget(self.voice_header)
        side_layout.addWidget(self.share_button)
        side_layout.addWidget(self.mute_button)
        side_layout.addWidget(self.deafen_button)
        side_layout.addWidget(self.noise_button)
        side_layout.addWidget(self.conn_button)

        self.tabs = QTabWidget()
        central = QWidget()
        layout = QHBoxLayout(central)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        layout.addWidget(side)
        layout.addWidget(self.tabs)
        self.setCentralWidget(central)

        initial_status = radmin_notice if radmin_notice else t("no_connection")
        self.status_label = QLabel(initial_status)
        self.e2e_badge = QLabel(t("e2e_footer"))
        self.e2e_badge.setObjectName("e2eFooter")
        self.audio_label = QLabel(t("voice_inactive") if self.audio.available else t("voice_missing_dll"))
        self.statusBar().addWidget(self.status_label, 1)
        self.statusBar().addPermanentWidget(self.e2e_badge)
        self.statusBar().addPermanentWidget(self.audio_label)

    def _on_mute_toggled(self, checked: bool):
        self.audio.set_mic_muted(checked)
        self.mute_button.setText(t("btn_unmute_mic") if checked else t("btn_mute_mic"))

    def _on_deafen_toggled(self, checked: bool):
        self.audio.set_deafened(checked)
        self.deafen_button.setText(t("btn_undeafen") if checked else t("btn_deafen"))

    def _on_noise_toggled(self, checked: bool):
        self.audio.set_noise_suppression(checked)
        self.noise_button.setText(t("btn_noise_on") if checked else t("btn_noise_off"))

    def retranslate_ui(self):
        """Atualiza dinamicamente todos os textos da interface após mudança de idioma."""
        self.setWindowTitle(t("room_title", name=self.last_name) if self.last_name else t("room_title_default"))
        self.conn_button.setText(t("btn_connection"))
        self.header.setText(t("sidebar_connected"))
        self.voice_header.setText(t("sidebar_voice_screen"))
        is_me = self.last_name in self.active_streamers
        self.share_button.setText(t("btn_stop_stream") if is_me else t("btn_share_screen"))
        self.mute_button.setText(t("btn_unmute_mic") if self.mute_button.isChecked() else t("btn_mute_mic"))
        self.deafen_button.setText(t("btn_undeafen") if self.deafen_button.isChecked() else t("btn_deafen"))
        self.noise_button.setText(t("btn_noise_on") if self.noise_button.isChecked() else t("btn_noise_off"))
        self.noise_button.setToolTip(t("tooltip_noise_suppression"))

        if self.tabs.count() >= 4:
            self.tabs.setTabText(0, t("tab_screens_count", count=len(self.active_streamers)) if self.active_streamers else t("tab_screens"))
            self.tabs.setTabText(1, t("tab_chat"))
            self.tabs.setTabText(2, t("tab_tasks"))
            self.tabs.setTabText(3, t("tab_notes"))

        if not self.session:
            self.status_label.setText(t("no_connection"))
            self.e2e_badge.setText(t("e2e_footer"))
            self.audio_label.setText(t("voice_inactive") if self.audio.available else t("voice_missing_dll"))

    def toggle_loopback(self, enabled: bool):
        self.audio.set_loopback(enabled)
        if hasattr(self, "video_stage"):
            self.video_stage.set_loopback_checked(enabled)

    def _poll_speaking(self):
        if not self.session:
            if self._last_speaking_names:
                self._last_speaking_names.clear()
                self._apply_speaking_ui(set())
            return
        mask = self.audio.get_speaking_mask()
        speaking = {
            name
            for sid, name in self.sender_id_to_name.items()
            if 0 < sid < 32 and (mask & (1 << sid))
        }
        if speaking != self._last_speaking_names:
            self._last_speaking_names = speaking
            self._apply_speaking_ui(speaking)

    def _apply_speaking_ui(self, speaking_names: set[str]):
        for name, row_widget in self._peer_rows.items():
            row_widget.set_speaking(name in speaking_names)
        if hasattr(self, "video_stage"):
            self.video_stage.update_speaking(speaking_names)

    def toggle_screen_share(self):
        if not self.session:
            self.status_label.setText("Conecte-se a uma sala antes de compartilhar a tela")
            return
        if self.last_name in self.active_streamers:
            self.toggle_loopback(False)
            self.send({"t": "stream_stop"})
        else:
            if len(self.active_streamers) >= MAX_STREAMS:
                QMessageBox.information(
                    self,
                    "Limite de Transmissões",
                    "Já existem 2 pessoas transmitindo tela na sala.\n"
                    "Aguarde uma transmissão encerrar para iniciar a sua.",
                )
                return
            dlg = ScreenShareDialog(self, default_share_audio=True)
            if not dlg.exec():
                return
            cfg = dlg.values()
            share_audio = bool(cfg.pop("share_audio", True))
            self.video.configure_capture(**cfg)
            self.toggle_loopback(share_audio)
            audio_tag = " + Áudio do Sistema" if share_audio else ""
            self.status_label.setText(f"Iniciando apresentação: {cfg['label']}{audio_tag}")
            self.send({"t": "stream_start"})
            self.tabs.setCurrentIndex(0)

    def _on_video_frame(self, stream_id: int, img):
        if hasattr(self, "video_stage"):
            self.video_stage.push_frame(stream_id, img)

    def send(self, req) -> bool:
        if self.session and self.session.send(req):
            return True
        self.status_label.setText("Sem conexão: ação não enviada")
        return False

    def send_image(self, path, req) -> bool:
        if not (self.session and self.store):
            self.status_label.setText("Sem conexão: ação não enviada")
            return False
        store = self.store
        self.status_label.setText("Cifrando e enviando imagem...")

        def work():
            if os.path.getsize(path) > MAX_BYTES:
                return None
            data = Path(path).read_bytes()
            if not store.save_media(data):
                return None
            return base64.b64encode(data).decode("ascii")

        def done(b64):
            if b64 is None:
                self.status_label.setText("Imagem inválida ou maior que 10 MB")
            elif not self.send({**req, "img": {"data": b64}}):
                self.status_label.setText("Sem conexão: imagem não enviada")

        run_async(work, done)
        return True

    def on_media_ready(self):
        self.chat.refresh_media()
        self.tasks.refresh()

    def connect_dialog(self):
        dlg = ConnectDialog(self, self.last_name, self.last_password, radmin_notice=self.radmin_notice)
        if dlg.exec():
            self.start_session(**dlg.values())

    def start_session(self, name, host_mode, address, port, password=""):
        self.stop_session()
        self.last_name = name
        self.last_password = password
        self.store = Store(name)
        self.sender_id_to_name = {}

        base_udp = DEFAULT_UDP_PORT if port == 45000 else port + 1
        ok_audio, audio_msg = self.audio.start(base_udp)
        udp_port = self.audio.active_port or base_udp

        base_vid = DEFAULT_VIDEO_PORT if port == 45000 else port + 2
        ok_vid, _ = self.video.start(base_vid)
        video_port = self.video.active_port or base_vid
        self.audio_label.setText(f"{audio_msg} | Vídeo: {video_port if ok_vid else 'off'}")

        if host_mode:
            self.session = HostSession(self.store, name, port, password, udp_port, video_port, self)
            self.sender_id_to_name[1] = name
            self.audio.set_key(self.session.keys.audio_key, self.session.sender_id)
            self.video.set_key(self.session.keys.video_key, self.session.sender_id)
            self.e2e_badge.setText("Conexão Cifrada (E2EE: AES-256-GCM / Argon2id)")
        else:
            self.session = ClientSession(self.store, name, address, port, password, udp_port, video_port, self)
            self.session.authenticated.connect(self._on_client_authenticated)
            self.session.host_disconnected.connect(self._on_host_disconnected)
            self.session.kicked.connect(self._on_client_kicked)
            self.session.server_muted.connect(self._on_client_server_muted)
            self.e2e_badge.setText("Verificando chave da sala (E2EE)...")

        self.session.status.connect(self.status_label.setText)
        self.session.peers_changed.connect(self.show_peers)
        self.session.endpoints_changed.connect(self.on_endpoints_changed)
        self.session.streams_changed.connect(self.on_streams_changed)
        self.session.applied.connect(self.on_event)
        if hasattr(self.session, "media_ready"):
            self.session.media_ready.connect(self.on_media_ready)
        self.setWindowTitle(f"LocalCall - {name}")

        self.tabs.clear()
        self.video_stage = VideoStage(
            self.toggle_screen_share,
            self.toggle_loopback,
            on_peer_context_menu=self.show_peer_context_menu,
        )
        self.video_stage.set_local_user(name)
        self.chat = ChatPanel(self.store, self.send, self.send_image, current_user=name, is_host=host_mode)
        self.tasks = TasksPanel(self.store, self.send, self.send_image)
        self.notes = NotesPanel(self.store, self.send)
        self.tabs.addTab(self.video_stage, t("tab_screens"))
        self.tabs.addTab(self.chat, t("tab_chat"))
        self.tabs.addTab(self.tasks, t("tab_tasks"))
        self.tabs.addTab(self.notes, t("tab_notes"))
        if not self.session.start() and host_mode:
            QMessageBox.warning(self, "Erro", self.status_label.text())

    def _on_client_authenticated(self, keys, sender_id: int):
        self.sender_id_to_name[int(sender_id)] = self.last_name
        self.audio.set_key(keys.audio_key, sender_id)
        self.video.set_key(keys.video_key, sender_id)
        self.e2e_badge.setText("Conexão Cifrada (E2EE: AES-256-GCM / Argon2id)")

    def _on_host_disconnected(self):
        """Trata queda do Radmin ou encerramento da sala pelo Host sem travar a UI."""
        self.active_streamers = []
        self.share_button.setText(t("btn_share_screen"))
        self.toggle_loopback(False)
        self.video.stop_capture()
        self.e2e_badge.setText(t("host_disconnected"))
        self.status_label.setText(t("host_disconnected"))

    def _on_client_kicked(self, reason: str):
        """Trata expulsão da sala pelo Host limpando recursos de áudio/vídeo e avisando o usuário."""
        self.active_streamers = []
        self.share_button.setText(t("btn_share_screen"))
        self.toggle_loopback(False)
        self.video.stop_capture()
        self.audio.cleanup()
        self.video.cleanup()
        msg = reason or t("msg_kicked_by_host")
        self.e2e_badge.setText(msg)
        self.status_label.setText(msg)
        QMessageBox.warning(self, t("msg_kicked_title"), msg)

    def _on_client_server_muted(self):
        """Força o mute do microfone local quando o Host aplica 'Silenciar para Todos'."""
        self.mute_button.setChecked(True)
        self.status_label.setText(t("msg_server_muted"))

    def _get_sender_id_for_name(self, peer_name: str) -> int | None:
        for sid, name in self.sender_id_to_name.items():
            if name == peer_name:
                return int(sid)
        return None

    def _refresh_peer_row_badge(self, peer_name: str):
        row = self._peer_rows.get(peer_name)
        if row:
            row.update_audio_state(
                self.audio.is_peer_muted(peer_name),
                self.audio.get_peer_volume(peer_name),
            )

    def _on_peers_list_context_menu(self, pos: QPoint):
        item = self.peers.itemAt(pos)
        if not item:
            return
        row = self.peers.itemWidget(item)
        peer_name = getattr(row, "peer_name", "") if row else item.text()
        if peer_name:
            self.show_peer_context_menu(peer_name, self.peers.viewport().mapToGlobal(pos))

    def show_peer_context_menu(self, peer_name: str, global_pos: QPoint):
        """Exibe o Menu de Contexto estilo Discord para ajuste de áudio individual e moderação do Host."""
        if not peer_name:
            return
        is_self = (peer_name == self.last_name)
        is_host = isinstance(self.session, HostSession)
        sid = self._get_sender_id_for_name(peer_name)

        menu = QMenu(self)

        # Cabeçalho com o nome do participante
        header_act = QAction(f"👤  {peer_name}", menu)
        header_act.setEnabled(False)
        menu.addAction(header_act)
        menu.addSeparator()

        # 1. Mutar para Mim (Local Mute)
        act_mute_local = QAction(t("ctx_mute_peer"), menu)
        act_mute_local.setCheckable(True)
        act_mute_local.setChecked(self.audio.is_peer_muted(peer_name))
        act_mute_local.setEnabled(not is_self)

        def _toggle_local_mute(checked: bool):
            self.audio.set_peer_muted(peer_name, checked, sender_id=sid)
            self._refresh_peer_row_badge(peer_name)

        act_mute_local.toggled.connect(_toggle_local_mute)
        menu.addAction(act_mute_local)

        # 2. Volume do Usuário (Slider 0% a 150%)
        vol_container = QWidget(menu)
        vol_layout = QVBoxLayout(vol_container)
        vol_layout.setContentsMargins(12, 6, 12, 6)
        vol_layout.setSpacing(4)

        current_vol = self.audio.get_peer_volume(peer_name)
        vol_label = QLabel(t("ctx_user_volume", pct=current_vol), vol_container)
        vol_label.setObjectName("chatMeta")

        vol_slider = QSlider(Qt.Orientation.Horizontal, vol_container)
        vol_slider.setRange(0, 150)
        vol_slider.setSingleStep(5)
        vol_slider.setPageStep(10)
        vol_slider.setValue(current_vol)
        vol_slider.setMinimumWidth(180)
        vol_slider.setEnabled(not is_self)

        def _on_vol_changed(val: int):
            vol_label.setText(t("ctx_user_volume", pct=val))
            self.audio.set_peer_volume(peer_name, val, sender_id=sid)
            self._refresh_peer_row_badge(peer_name)

        vol_slider.valueChanged.connect(_on_vol_changed)
        vol_layout.addWidget(vol_label)
        vol_layout.addWidget(vol_slider)

        vol_action = QWidgetAction(menu)
        vol_action.setDefaultWidget(vol_container)
        menu.addAction(vol_action)

        # 3. Mutar Áudio da Tela deste Usuário (Loopback Mute)
        act_mute_screen = QAction(t("ctx_mute_screen_audio"), menu)
        act_mute_screen.setCheckable(True)
        act_mute_screen.setChecked(self.audio.is_peer_loopback_muted(peer_name))
        act_mute_screen.setEnabled(not is_self)
        act_mute_screen.toggled.connect(
            lambda checked: self.audio.set_peer_loopback_muted(peer_name, checked, sender_id=sid)
        )
        menu.addAction(act_mute_screen)

        # 4. Opções Exclusivas do Host da Sala (Moderação)
        if is_host and not is_self:
            menu.addSeparator()
            mod_header = QAction(f"🛡️  {t('ctx_mod_header')}", menu)
            mod_header.setEnabled(False)
            menu.addAction(mod_header)

            act_server_mute = QAction(t("ctx_server_mute"), menu)
            act_server_mute.triggered.connect(
                lambda: self.send({"t": "server_mute", "target": peer_name})
            )
            menu.addAction(act_server_mute)

            act_kick = QAction(t("ctx_kick_peer"), menu)
            act_kick.triggered.connect(
                lambda: self.send({"t": "kick", "target": peer_name})
            )
            menu.addAction(act_kick)

        menu.exec(global_pos)

    def stop_session(self):
        self.active_streamers = []
        self.sender_id_to_name.clear()
        self._last_speaking_names.clear()
        self.share_button.setText(t("btn_share_screen"))
        if hasattr(self, "video_stage"):
            self.video_stage.set_loopback_checked(False)
        self.video.cleanup()
        self.audio.cleanup()
        if self.session:
            self.session.stop()
            self.session.deleteLater()
            self.session = None

    def show_peers(self, names):
        self.peers.clear()
        self._peer_rows.clear()
        for name in names:
            item = QListWidgetItem(self.peers)
            item.setSizeHint(QSize(180, 32))
            row = PeerRowWidget(
                name,
                speaking=(name in self._last_speaking_names),
                muted=self.audio.is_peer_muted(name),
                volume=self.audio.get_peer_volume(name),
                on_context_menu=self.show_peer_context_menu,
            )
            self.peers.addItem(item)
            self.peers.setItemWidget(item, row)
            self._peer_rows[name] = row
        if hasattr(self, "video_stage"):
            self.video_stage.update_peers(names)

    def on_endpoints_changed(self, endpoints):
        # Atualiza o mapa sender_id -> nome para o Green Ring
        if isinstance(self.session, HostSession):
            self.sender_id_to_name = {1: self.last_name}
        elif isinstance(self.session, ClientSession) and self.session.sender_id:
            self.sender_id_to_name = {int(self.session.sender_id): self.last_name}
        else:
            self.sender_id_to_name = {}

        for ep in endpoints:
            sid = ep.get("sender_id")
            ep_name = ep.get("name")
            if sid and ep_name:
                self.sender_id_to_name[int(sid)] = str(ep_name)

        self.audio.sync_peers(endpoints, self.last_name)
        self.video.sync_targets(endpoints, self.last_name)

    def on_streams_changed(self, active: list[str]):
        self.active_streamers = list(active)
        is_me = self.last_name in self.active_streamers
        self.share_button.setText(t("btn_stop_stream") if is_me else t("btn_share_screen"))
        if is_me and not self.video.capturing:
            self.video.start_capture()
        elif not is_me and self.video.capturing:
            self.video.stop_capture()
            self.toggle_loopback(False)

        if hasattr(self, "video_stage"):
            self.video_stage.update_streamers(self.active_streamers)
            if self.active_streamers:
                self.tabs.setTabText(0, t("tab_screens_count", count=len(self.active_streamers)))
            else:
                self.tabs.setTabText(0, t("tab_screens"))

    def on_event(self, ev):
        t = ev["t"]
        if t == "snapshot":
            for panel in (self.chat, self.tasks, self.notes):
                panel.refresh()
        elif t == "chat":
            self.chat.refresh()
            # Toca notificação sutil se a mensagem veio de outra pessoa e a janela está minimizada/sem foco ou em outra aba
            if ev.get("author") and ev.get("author") != self.last_name:
                if not self.isActiveWindow() or self.isMinimized() or self.tabs.currentWidget() is not self.chat:
                    play_chat_ping()
        elif t == "chat_del":
            self.chat.refresh()
        elif t.startswith("task"):
            self.tasks.refresh()
        elif t.startswith("note"):
            self.notes.refresh()

    def closeEvent(self, event):
        self._vad_timer.stop()
        self.stop_session()
        super().closeEvent(event)
