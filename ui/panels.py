"""Painéis de produtividade: chat, tarefas e notas."""
from datetime import datetime
from pathlib import Path

from PyQt6.QtCore import QSize, Qt, QTimer
from PyQt6.QtGui import QIcon
from PyQt6.QtWidgets import (QFrame, QHBoxLayout, QLabel, QLineEdit, QListWidget,
                             QListWidgetItem, QMenu, QPlainTextEdit, QPushButton,
                             QScrollArea, QSplitter, QVBoxLayout, QWidget)

from .media_widgets import MediaLabel, ViewerDialog, load_pixmap, pick_image
from .sound_fx import play_pomodoro_chime
from .i18n import t

ID_ROLE = Qt.ItemDataRole.UserRole
IMG_ROLE = Qt.ItemDataRole.UserRole + 1
TASK_ICON = QSize(96, 72)


class ChatPanel(QWidget):
    def __init__(self, store, send, send_image, current_user="", is_host=False):
        super().__init__()
        self.store, self.send, self.send_image = store, send, send_image
        self.current_user = current_user
        self.is_host = is_host
        self.shown = []        # ids já desenhados (só novas mensagens são adicionadas)
        self.thumbs = []
        self.replying_to = None  # dict {"id": int, "author": str, "text": str}

        self.body = QWidget()
        self.body.setObjectName("chatBody")
        self.col = QVBoxLayout(self.body)
        self.col.setContentsMargins(12, 10, 12, 10)
        self.col.setSpacing(10)
        self.col.addStretch()
        self.scroll = QScrollArea()
        self.scroll.setWidgetResizable(True)
        self.scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.scroll.setWidget(self.body)

        # Barra de resposta acima da caixa de digitação
        self.reply_bar = QFrame()
        self.reply_bar.setObjectName("replyBar")
        self.reply_bar.hide()
        reply_layout = QHBoxLayout(self.reply_bar)
        reply_layout.setContentsMargins(10, 6, 8, 6)
        reply_layout.setSpacing(8)
        self.reply_label = QLabel()
        self.reply_label.setObjectName("replyBarText")
        self.reply_label.setTextFormat(Qt.TextFormat.PlainText)
        cancel_reply = QPushButton("X")
        cancel_reply.setObjectName("replyCancelBtn")
        cancel_reply.setFixedSize(24, 22)
        cancel_reply.setCursor(Qt.CursorShape.PointingHandCursor)
        cancel_reply.clicked.connect(self._clear_reply)
        reply_layout.addWidget(self.reply_label, 1)
        reply_layout.addWidget(cancel_reply)

        self.input = QLineEdit()
        self.input.setPlaceholderText(t("chat_placeholder"))
        self.input.returnPressed.connect(self._submit)
        self.attach_btn = QPushButton(t("btn_image"))
        self.attach_btn.clicked.connect(self._attach)
        self.submit_btn = QPushButton(t("btn_send"))
        self.submit_btn.setProperty("primary", True)
        self.submit_btn.clicked.connect(self._submit)

        row = QHBoxLayout()
        row.addWidget(self.input)
        row.addWidget(self.attach_btn)
        row.addWidget(self.submit_btn)

        self.e2e_footer = QLabel(t("e2e_chat_notice"))
        self.e2e_footer.setObjectName("e2eFooter")
        self.e2e_footer.setAlignment(Qt.AlignmentFlag.AlignCenter)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 12, 12, 8)
        layout.addWidget(self.scroll)
        layout.addWidget(self.reply_bar)
        layout.addLayout(row)
        layout.addWidget(self.e2e_footer)
        self.refresh()

    def _set_reply(self, m: dict):
        """Ativa o modo de resposta para a mensagem selecionada."""
        snippet = (m.get("text") or "").strip().replace("\n", " ")
        if not snippet and m.get("image"):
            snippet = t("image_tag")
        if len(snippet) > 70:
            snippet = snippet[:67] + "..."
        self.replying_to = {"id": m["id"], "author": m["author"], "text": snippet}
        self.reply_label.setText(t("replying_to", author=m["author"], text=snippet))
        self.reply_bar.show()
        self.input.setFocus()

    def _clear_reply(self):
        self.replying_to = None
        self.reply_bar.hide()

    def _submit(self):
        text = self.input.text().strip()
        if not text:
            return
        req = {"t": "chat", "text": text}
        if self.replying_to:
            req["reply_to"] = self.replying_to
        if self.send(req):
            self.input.clear()
            self._clear_reply()

    def _attach(self):
        """Escolhe a imagem e envia na hora, usando o texto digitado como legenda."""
        path = pick_image(self)
        if not path:
            return
        req = {"t": "chat", "text": self.input.text().strip()}
        if self.replying_to:
            req["reply_to"] = self.replying_to
        if self.send_image(path, req):
            self.input.clear()
            self._clear_reply()

    def refresh(self):
        rows = self.store.chat()
        ids = [m["id"] for m in rows]
        if ids[:len(self.shown)] != self.shown:    # estado trocado (snapshot): redesenha
            self._clear()
        for m in rows[len(self.shown):]:
            self._add(m)
        self._scroll_down()

    def refresh_media(self):
        """Carrega as imagens que estavam aguardando download."""
        for thumb in self.thumbs:
            if not thumb.loaded:
                thumb.load()

    def _clear(self):
        while self.col.count() > 1:
            self.col.takeAt(0).widget().deleteLater()
        self.shown, self.thumbs = [], []

    def _resolve_quote(self, m: dict):
        """Obtém autor e trecho da mensagem citada (do próprio registro ou buscando no banco)."""
        rid = m.get("reply_to_id")
        if rid is None:
            return None
        author = m.get("reply_author")
        text = m.get("reply_text")
        if not author:
            orig = self.store.get("chat", rid)
            if orig:
                author = orig["author"]
                text = orig["text"] or ("[Imagem]" if orig.get("image") else "")
            else:
                author = "Mensagem"
                text = "..."
        snippet = (text or "").replace("\n", " ")
        if len(snippet) > 85:
            snippet = snippet[:82] + "..."
        return author, snippet

    def _add(self, m):
        box = QWidget()
        box.setObjectName("msg")
        lay = QVBoxLayout(box)
        lay.setContentsMargins(6, 4, 6, 4)
        lay.setSpacing(3)

        # Citação/Reply renderizada acima da mensagem (estilo Discord/WhatsApp)
        quote = self._resolve_quote(m)
        if quote:
            q_author, q_text = quote
            q_frame = QFrame()
            q_frame.setObjectName("replyQuote")
            q_lay = QHBoxLayout(q_frame)
            q_lay.setContentsMargins(8, 3, 8, 3)
            q_lay.setSpacing(6)
            q_lbl = QLabel(f"{q_author}: {q_text}")
            q_lbl.setObjectName("replyQuoteText")
            q_lbl.setTextFormat(Qt.TextFormat.PlainText)
            q_lbl.setWordWrap(True)
            q_lay.addWidget(q_lbl)
            lay.addWidget(q_frame)

        # Cabeçalho da mensagem com botão discreto "Responder"
        header_row = QHBoxLayout()
        header_row.setContentsMargins(0, 0, 0, 0)
        header_row.setSpacing(8)
        hour = datetime.fromtimestamp(m["ts"]).strftime("%H:%M")
        meta = QLabel(f"{m['author']}  {hour}")
        meta.setObjectName("chatMeta")
        meta.setTextFormat(Qt.TextFormat.PlainText)

        reply_btn = QPushButton(t("btn_reply"))
        reply_btn.setObjectName("msgReplyBtn")
        reply_btn.setFlat(True)
        reply_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        reply_btn.clicked.connect(lambda _=False, msg=dict(m): self._set_reply(msg))

        header_row.addWidget(meta)
        header_row.addWidget(reply_btn)
        header_row.addStretch()
        lay.addLayout(header_row)

        if m["text"]:
            text = QLabel(m["text"])
            text.setTextFormat(Qt.TextFormat.PlainText)
            text.setWordWrap(True)
            text.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
            lay.addWidget(text)
        if m.get("image"):
            thumb = MediaLabel(self.store, m["image"])
            self.thumbs.append(thumb)
            lay.addWidget(thumb, 0, Qt.AlignmentFlag.AlignLeft)

        # Menu de contexto com botão direito também permite responder
        box.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        box.customContextMenuRequested.connect(
            lambda pos, w=box, msg=dict(m): self._show_msg_menu(w, pos, msg)
        )

        self.col.insertWidget(self.col.count() - 1, box)
        self.shown.append(m["id"])

    def _delete_msg(self, m: dict):
        """Envia solicitação para apagar a mensagem."""
        msg_id = m.get("id")
        if msg_id is not None:
            self.send({"t": "chat_del", "id": msg_id})

    def _show_msg_menu(self, widget, pos, msg: dict):
        menu = QMenu(self)
        act_reply = menu.addAction(t("menu_reply"))
        act_del = None
        # Permite apagar se for o autor da mensagem ou o host da sala
        if self.is_host or (self.current_user and msg.get("author") == self.current_user):
            act_del = menu.addAction(t("menu_delete"))

        chosen = menu.exec(widget.mapToGlobal(pos))
        if chosen == act_reply:
            self._set_reply(msg)
        elif chosen is not None and chosen == act_del:
            self._delete_msg(msg)

    def _scroll_down(self):
        bar = self.scroll.verticalScrollBar()
        QTimer.singleShot(30, lambda: bar.setValue(bar.maximum()))


class TasksPanel(QWidget):
    def __init__(self, store, send, send_image):
        super().__init__()
        self.store, self.send, self.send_image = store, send, send_image
        self.pending = ""      # imagem escolhida para a próxima tarefa

        # --- Barra de Pomodoro integrada às Tarefas ---
        self._pomo_focus_sec = 25 * 60
        self._pomo_break_sec = 5 * 60
        self._pomo_remaining = self._pomo_focus_sec
        self._pomo_is_break = False
        self._pomo_timer = QTimer(self)
        self._pomo_timer.setInterval(1000)
        self._pomo_timer.timeout.connect(self._tick_pomodoro)

        pomo_bar = QFrame()
        pomo_bar.setObjectName("qualityPanel")
        pomo_lay = QHBoxLayout(pomo_bar)
        pomo_lay.setContentsMargins(10, 6, 10, 6)
        pomo_lay.setSpacing(8)
        self.pomo_mode_lbl = QLabel(t("pomo_focus"))
        self.pomo_mode_lbl.setObjectName("sectionLabel")
        self.pomo_time_lbl = QLabel("25:00")
        self.pomo_time_lbl.setObjectName("pomoTimer")
        self.pomo_start_btn = QPushButton(t("btn_start"))
        self.pomo_start_btn.clicked.connect(self._toggle_pomodoro)
        self.pomo_reset_btn = QPushButton(t("btn_reset"))
        self.pomo_reset_btn.clicked.connect(self._reset_pomodoro)
        pomo_lay.addWidget(self.pomo_mode_lbl)
        pomo_lay.addWidget(self.pomo_time_lbl)
        pomo_lay.addStretch()
        pomo_lay.addWidget(self.pomo_start_btn)
        pomo_lay.addWidget(self.pomo_reset_btn)

        self.list = QListWidget()
        self.list.setIconSize(TASK_ICON)
        self.list.itemChanged.connect(self._toggled)
        self.list.itemDoubleClicked.connect(self._open_image)
        self.input = QLineEdit()
        self.input.setPlaceholderText(t("task_placeholder"))
        self.input.returnPressed.connect(self._add)
        attach = QPushButton(t("btn_image"))
        attach.clicked.connect(self._attach)
        add = QPushButton(t("btn_add"))
        add.setProperty("primary", True)
        add.clicked.connect(self._add)
        self.hint = QLabel(t("hint_task_image"))
        self.hint.setObjectName("hint")
        attach_sel = QPushButton(t("btn_attach_selected"))
        attach_sel.clicked.connect(self._attach_selected)
        remove = QPushButton(t("btn_remove_selected"))
        remove.clicked.connect(self._remove)

        row = QHBoxLayout()
        row.addWidget(self.input)
        row.addWidget(attach)
        row.addWidget(add)
        row2 = QHBoxLayout()
        row2.addWidget(self.hint, 1)
        row2.addWidget(attach_sel)
        row2.addWidget(remove)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.addWidget(pomo_bar)
        layout.addWidget(self.list)
        layout.addLayout(row)
        layout.addLayout(row2)
        self.refresh()

    def _update_pomo_display(self):
        mins, secs = divmod(max(0, self._pomo_remaining), 60)
        self.pomo_time_lbl.setText(f"{mins:02d}:{secs:02d}")
        self.pomo_mode_lbl.setText(t("pomo_break") if self._pomo_is_break else t("pomo_focus"))

    def _toggle_pomodoro(self):
        if self._pomo_timer.isActive():
            self._pomo_timer.stop()
            self.pomo_start_btn.setText(t("btn_resume"))
        else:
            self._pomo_timer.start()
            self.pomo_start_btn.setText(t("btn_pause"))

    def _reset_pomodoro(self):
        self._pomo_timer.stop()
        self._pomo_is_break = False
        self._pomo_remaining = self._pomo_focus_sec
        self.pomo_start_btn.setText(t("btn_start"))
        self._update_pomo_display()

    def _tick_pomodoro(self):
        self._pomo_remaining -= 1
        if self._pomo_remaining <= 0:
            play_pomodoro_chime()
            self._pomo_is_break = not self._pomo_is_break
            self._pomo_remaining = self._pomo_break_sec if self._pomo_is_break else self._pomo_focus_sec
        self._update_pomo_display()

    def _attach(self):
        path = pick_image(self)
        if path:
            self.pending = path
            self.hint.setText(t("hint_task_attached", name=Path(path).name))

    def _add(self):
        text = self.input.text().strip()
        if not text:
            return
        req = {"t": "task_add", "text": text}
        ok = self.send_image(self.pending, req) if self.pending else self.send(req)
        if ok:
            self.input.clear()
            self.pending = ""
            self.hint.setText(t("hint_task_image"))

    def _attach_selected(self):
        item = self.list.currentItem()
        if not item:
            return
        path = pick_image(self)
        if path:
            self.send_image(path, {"t": "task_image", "id": item.data(ID_ROLE)})

    def _open_image(self, item):
        name = item.data(IMG_ROLE)
        if name and self.store.has_media(name):
            ViewerDialog(self.store.media_path(name), self.window()).exec()

    def _remove(self):
        item = self.list.currentItem()
        if item:
            self.send({"t": "task_del", "id": item.data(ID_ROLE)})

    def _toggled(self, item):
        self.send({"t": "task_toggle", "id": item.data(ID_ROLE)})

    def refresh(self):
        self.list.blockSignals(True)   # evita disparar toggle ao repopular
        selected = self.list.currentItem().data(ID_ROLE) if self.list.currentItem() else None
        self.list.clear()
        for t in self.store.tasks():
            item = QListWidgetItem(f"{t['text']}   ({t['author']})")
            item.setData(ID_ROLE, t["id"])
            item.setData(IMG_ROLE, t.get("image"))
            item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
            item.setCheckState(Qt.CheckState.Checked if t["done"] else Qt.CheckState.Unchecked)
            if self.store.has_media(t.get("image")):
                pix = load_pixmap(self.store.media_path(t["image"]), TASK_ICON)
                item.setIcon(QIcon(pix))
            self.list.addItem(item)
            if t["id"] == selected:
                self.list.setCurrentItem(item)
        self.list.blockSignals(False)


class NotesPanel(QWidget):
    def __init__(self, store, send):
        super().__init__()
        self.store, self.send = store, send
        self.current_id = None
        self.dirty = False

        self.list = QListWidget()
        self.list.currentItemChanged.connect(self._select)
        new = QPushButton(t("btn_new_note"))
        new.clicked.connect(self._new)
        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.setContentsMargins(0, 0, 0, 0)
        left_layout.addWidget(self.list)
        left_layout.addWidget(new)

        self.title = QLineEdit()
        self.title.setPlaceholderText(t("placeholder_note_title"))
        self.body = QPlainTextEdit()
        self.info = QPushButton()     # só exibe quem editou por último
        self.info.setFlat(True)
        self.info.setEnabled(False)
        save = QPushButton(t("btn_save"))
        save.setProperty("primary", True)
        save.clicked.connect(self._save)
        delete = QPushButton(t("btn_delete"))
        delete.clicked.connect(self._delete)
        for edit in (self.title, self.body):
            edit.textChanged.connect(self._mark_dirty)

        buttons = QHBoxLayout()
        buttons.addWidget(self.info, 1)
        buttons.addWidget(delete)
        buttons.addWidget(save)
        right = QWidget()
        right_layout = QVBoxLayout(right)
        right_layout.setContentsMargins(0, 0, 0, 0)
        right_layout.addWidget(self.title)
        right_layout.addWidget(self.body)
        right_layout.addLayout(buttons)

        split = QSplitter()
        split.addWidget(left)
        split.addWidget(right)
        split.setSizes([200, 600])
        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.addWidget(split)
        self.refresh()

    def _mark_dirty(self):
        if not self._loading:
            self.dirty = True

    _loading = False

    def _load(self, note):
        """Carrega uma nota no editor (ou limpa, se for None)."""
        self._loading = True
        self.current_id = note["id"] if note else None
        self.title.setText(note["title"] if note else "")
        self.body.setPlainText(note["body"] if note else "")
        self.info.setText(t("last_edit", author=note['author']) if note else "")
        self._loading = False
        self.dirty = False

    def _select(self, item):
        self._load(self.store.get("notes", item.data(ID_ROLE)) if item else None)

    def _new(self):
        self.list.setCurrentItem(None)
        self._load(None)
        self.title.setFocus()

    def _save(self):
        if self.send({"t": "note_save", "id": self.current_id,
                      "title": self.title.text().strip(), "body": self.body.toPlainText()}):
            self.dirty = False

    def _delete(self):
        if self.current_id is not None:
            self.send({"t": "note_del", "id": self.current_id})

    def refresh(self):
        self.list.blockSignals(True)
        self.list.clear()
        target = None
        for n in self.store.notes():
            item = QListWidgetItem(n["title"])
            item.setData(ID_ROLE, n["id"])
            self.list.addItem(item)
            if n["id"] == self.current_id:
                target = item
        if target:
            self.list.setCurrentItem(target)
        self.list.blockSignals(False)
        # Nota aberta: recarrega se mudou e não há edição local pendente
        note = self.store.get("notes", self.current_id) if self.current_id is not None else None
        if self.current_id is not None and note is None:
            self._load(None)           # foi excluída
        elif note and not self.dirty:
            self._load(note)
