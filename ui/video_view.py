"""Área de visualização de compartilhamento de tela em grade adaptativa com modo Tela Cheia."""
from PyQt6.QtCore import Qt
from PyQt6.QtGui import QColor, QFont, QImage, QPainter
from PyQt6.QtWidgets import (QDialog, QFrame, QGridLayout, QHBoxLayout, QLabel,
                             QPushButton, QSizePolicy, QVBoxLayout, QWidget)

from .i18n import t


def _paint_video_surface(widget: QWidget, image: QImage | None, title: str, is_fullscreen: bool = False):
    painter = QPainter(widget)
    painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, True)
    rect = widget.rect()
    painter.fillRect(rect, QColor("#0d0d10"))

    res_badge = ""
    if image and not image.isNull():
        iw, ih = image.width(), image.height()
        res_badge = f"  [{iw}x{ih}]"
        scaled = image.scaled(
            rect.size(),
            Qt.AspectRatioMode.KeepAspectRatio,
            Qt.TransformationMode.SmoothTransformation,
        )
        x = (rect.width() - scaled.width()) // 2
        y = (rect.height() - scaled.height()) // 2
        painter.drawImage(x, y, scaled)
    else:
        painter.setPen(QColor("#a1a1aa"))
        painter.setFont(QFont("Segoe UI", 10))
        painter.drawText(rect, Qt.AlignmentFlag.AlignCenter, t("waiting_video", title=title))

    if title:
        fs_hint = t("fullscreen_hint") if is_fullscreen else t("window_fullscreen_hint")
        full_tag = f"{title}{res_badge}{fs_hint}"
        tag_h = 26
        tag_w = min(rect.width() - 16, max(140, len(full_tag) * 7 + 28))
        tag_rect = rect.adjusted(8, rect.height() - tag_h - 8, -(rect.width() - 8 - tag_w), -8)
        painter.fillRect(tag_rect, QColor(24, 24, 27, 215))
        painter.setPen(QColor("#f4f4f5"))
        painter.setFont(QFont("Segoe UI", 9, QFont.Weight.DemiBold))
        painter.drawText(
            tag_rect.adjusted(8, 0, -8, 0),
            Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft,
            full_tag,
        )


class FullscreenVideoDialog(QDialog):
    """Janela de tela cheia dedicada para assistir à transmissão (ESC ou duplo clique fecha)."""

    def __init__(self, title: str, initial_image: QImage | None = None, parent=None):
        super().__init__(parent)
        self.title = title
        self._image = initial_image
        self.setWindowTitle(f"Tela Cheia - {title}")
        self.setWindowFlags(Qt.WindowType.Window | Qt.WindowType.FramelessWindowHint)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

    def set_frame(self, img: QImage | None):
        self._image = img
        self.update()

    def paintEvent(self, event):
        _paint_video_surface(self, self._image, self.title, is_fullscreen=True)

    def mouseDoubleClickEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.close()

    def keyPressEvent(self, event):
        if event.key() == Qt.Key.Key_Escape:
            self.close()
        else:
            super().keyPressEvent(event)


class VideoSurface(QWidget):
    """Renderiza frames mantendo a proporção (16:9 / 21:9) e abre Tela Cheia com duplo clique."""

    def __init__(self, title: str = "", peer_name: str = "", on_context_menu=None):
        super().__init__()
        self.title = title
        self.peer_name = peer_name
        self.on_context_menu = on_context_menu
        self._image: QImage | None = None
        self._fs_dialog: FullscreenVideoDialog | None = None
        self.setMinimumSize(280, 180)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setToolTip("Duplo clique para alternar modo Tela Cheia (ESC para sair)")

    def contextMenuEvent(self, event):
        if self.on_context_menu and self.peer_name:
            self.on_context_menu(self.peer_name, event.globalPos())
            event.accept()
        else:
            super().contextMenuEvent(event)

    def set_frame(self, img: QImage):
        self._image = img
        self.update()
        if self._fs_dialog and self._fs_dialog.isVisible():
            self._fs_dialog.set_frame(img)

    def clear_frame(self):
        self._image = None
        self.update()
        if self._fs_dialog and self._fs_dialog.isVisible():
            self._fs_dialog.close()

    def toggle_fullscreen(self):
        if self._fs_dialog and self._fs_dialog.isVisible():
            self._fs_dialog.close()
            self._fs_dialog = None
            return
        self._fs_dialog = FullscreenVideoDialog(self.title, self._image, self.window())
        self._fs_dialog.showFullScreen()
        self._fs_dialog.activateWindow()
        self._fs_dialog.setFocus()

    def mouseDoubleClickEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.toggle_fullscreen()

    def keyPressEvent(self, event):
        if event.key() == Qt.Key.Key_Escape and self._fs_dialog and self._fs_dialog.isVisible():
            self._fs_dialog.close()
        else:
            super().keyPressEvent(event)

    def paintEvent(self, event):
        _paint_video_surface(self, self._image, self.title, is_fullscreen=False)

    def closeEvent(self, event):
        if self._fs_dialog and self._fs_dialog.isVisible():
            self._fs_dialog.close()
        super().closeEvent(event)


class PeerCard(QFrame):
    """Card exibido quando ninguém está transmitindo tela, com indicador Green Ring de fala."""

    def __init__(self, name: str, is_streaming: bool = False, is_speaking: bool = False, on_context_menu=None):
        super().__init__()
        self.peer_name = name
        self.on_context_menu = on_context_menu
        self.setObjectName("peerCard")
        self.setMinimumSize(180, 110)
        lay = QVBoxLayout(self)
        lay.setAlignment(Qt.AlignmentFlag.AlignCenter)

        self.initial = QLabel((name[:2] if name else "?").upper())
        self.initial.setObjectName("peerInitial")
        self.initial.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.initial.setFixedSize(48, 48)

        lbl_name = QLabel(name)
        lbl_name.setObjectName("peerCardName")
        lbl_name.setAlignment(Qt.AlignmentFlag.AlignCenter)

        self.status = QLabel(t("peer_status_streaming") if is_streaming else t("peer_status_voice"))
        self.status.setObjectName("chatMeta")
        self.status.setAlignment(Qt.AlignmentFlag.AlignCenter)

        lay.addWidget(self.initial, 0, Qt.AlignmentFlag.AlignCenter)
        lay.addWidget(lbl_name)
        lay.addWidget(self.status)
        self.set_speaking(is_speaking)

    def contextMenuEvent(self, event):
        if self.on_context_menu and self.peer_name:
            self.on_context_menu(self.peer_name, event.globalPos())
            event.accept()
        else:
            super().contextMenuEvent(event)

    def set_speaking(self, speaking: bool):
        self.setProperty("speaking", speaking)
        self.initial.setProperty("speaking", speaking)
        self.style().unpolish(self)
        self.style().polish(self)
        self.initial.style().unpolish(self.initial)
        self.initial.style().polish(self.initial)


class VideoStage(QWidget):
    """
    Grade adaptativa de telas:
    - 0 streams: cards dos participantes conectados (com Green Ring ao falar)
    - 1 stream: tela única expandida em modo foco/cinema
    - 2 streams: divisão lado a lado (split view)
    """

    def __init__(self, on_toggle_stream, on_toggle_loopback=None, on_peer_context_menu=None):
        super().__init__()
        self.on_toggle_stream = on_toggle_stream
        self.on_toggle_loopback = on_toggle_loopback
        self.on_peer_context_menu = on_peer_context_menu
        self.local_name = ""
        self.peers: list[str] = []
        self.streamers: list[str] = []
        self.speaking_names: set[str] = set()
        self.surfaces: dict[str, VideoSurface] = {}
        self.peer_cards: dict[str, PeerCard] = {}

        # Barra superior de controle de transmissão e loopback de áudio
        top_bar = QHBoxLayout()
        top_bar.setContentsMargins(0, 0, 0, 6)
        self.info_label = QLabel(t("no_active_streams"))
        self.info_label.setObjectName("hint")

        self.loopback_btn = QPushButton(t("sys_audio_off"))
        self.loopback_btn.setCheckable(True)
        self.loopback_btn.setToolTip(t("tooltip_sys_audio"))
        self.loopback_btn.toggled.connect(self._on_loopback_toggled)

        self.stream_btn = QPushButton(t("btn_share_screen"))
        self.stream_btn.setProperty("primary", True)
        self.stream_btn.clicked.connect(self.on_toggle_stream)

        top_bar.addWidget(self.info_label, 1)
        top_bar.addWidget(self.loopback_btn)
        top_bar.addWidget(self.stream_btn)

        self.stage_frame = QFrame()
        self.stage_frame.setObjectName("videoStage")
        self.grid = QGridLayout(self.stage_frame)
        self.grid.setContentsMargins(10, 10, 10, 10)
        self.grid.setSpacing(10)

        root = QVBoxLayout(self)
        root.setContentsMargins(12, 12, 12, 12)
        root.setSpacing(8)
        root.addLayout(top_bar)
        root.addWidget(self.stage_frame, 1)

    def _on_loopback_toggled(self, checked: bool):
        self.loopback_btn.setText(t("sys_audio_on") if checked else t("sys_audio_off"))
        if self.on_toggle_loopback:
            self.on_toggle_loopback(checked)

    def set_loopback_checked(self, checked: bool):
        self.loopback_btn.blockSignals(True)
        self.loopback_btn.setChecked(checked)
        self.loopback_btn.setText(t("sys_audio_on") if checked else t("sys_audio_off"))
        self.loopback_btn.blockSignals(False)

    def set_local_user(self, name: str):
        self.local_name = name

    def update_peers(self, names: list[str]):
        self.peers = list(names)
        if not self.streamers:
            self._rebuild_grid()

    def update_speaking(self, active_names: set[str]):
        self.speaking_names = set(active_names)
        for name, card in self.peer_cards.items():
            card.set_speaking(name in self.speaking_names)

    def update_streamers(self, streamers: list[str]):
        self.streamers = list(streamers)[:2]
        is_me = self.local_name in self.streamers
        self.stream_btn.setText(t("btn_stop_stream") if is_me else t("btn_share_screen"))
        self.stream_btn.setProperty("primary", not is_me)
        self.stream_btn.style().unpolish(self.stream_btn)
        self.stream_btn.style().polish(self.stream_btn)

        if not self.streamers:
            self.info_label.setText(t("no_active_streams"))
        else:
            nomes = ", ".join(self.streamers)
            self.info_label.setText(t("active_streams", count=len(self.streamers), names=nomes))

        self._rebuild_grid()

    def push_frame(self, stream_id: int, img: QImage):
        """Direciona o frame recebido para a superfície correspondente."""
        if not self.streamers:
            return
        if stream_id == 0 and self.local_name in self.surfaces:
            self.surfaces[self.local_name].set_frame(img)
            return

        remote_names = [n for n in self.streamers if n != self.local_name]
        if not remote_names:
            return
        idx = 0 if stream_id <= 1 else min(stream_id - 1, len(remote_names) - 1)
        target_name = remote_names[idx]
        if target_name in self.surfaces:
            self.surfaces[target_name].set_frame(img)

    def _clear_grid(self):
        for surf in self.surfaces.values():
            surf.clear_frame()
        while self.grid.count():
            item = self.grid.takeAt(0)
            w = item.widget()
            if w:
                w.setParent(None)
                w.deleteLater()
        self.surfaces.clear()
        self.peer_cards.clear()

    def _rebuild_grid(self):
        self._clear_grid()
        n_streams = len(self.streamers)

        if n_streams == 0:
            names = self.peers if self.peers else ([self.local_name] if self.local_name else [])
            for idx, name in enumerate(names):
                r, c = divmod(idx, 2)
                card = PeerCard(
                    name,
                    False,
                    is_speaking=(name in self.speaking_names),
                    on_context_menu=self.on_peer_context_menu,
                )
                self.peer_cards[name] = card
                self.grid.addWidget(card, r, c)
        elif n_streams == 1:
            name = self.streamers[0]
            surf = VideoSurface(
                f"{name} (Ao vivo)",
                peer_name=name,
                on_context_menu=self.on_peer_context_menu,
            )
            self.surfaces[name] = surf
            self.grid.addWidget(surf, 0, 0)
        else:
            for idx, name in enumerate(self.streamers[:2]):
                surf = VideoSurface(
                    f"{name} (Ao vivo)",
                    peer_name=name,
                    on_context_menu=self.on_peer_context_menu,
                )
                self.surfaces[name] = surf
                self.grid.addWidget(surf, 0, idx)
