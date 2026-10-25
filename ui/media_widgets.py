"""Widgets de imagem: miniatura (PNG/JPG/GIF animado) e visualizador ampliado."""
import os
from pathlib import Path

from PyQt6.QtCore import QSize, Qt
from PyQt6.QtGui import QImageReader, QMovie, QPixmap
from PyQt6.QtWidgets import (QApplication, QDialog, QFileDialog, QLabel, QMessageBox,
                             QSizePolicy, QVBoxLayout)

from database.media import MAX_BYTES


def pick_image(parent) -> str:
    """Abre o seletor de arquivos; devolve o caminho ou '' se cancelado/grande demais."""
    path, _ = QFileDialog.getOpenFileName(parent, "Selecionar imagem", "",
                                          "Imagens (*.png *.jpg *.jpeg *.gif)")
    if path and os.path.getsize(path) > MAX_BYTES:
        QMessageBox.warning(parent, "Imagem grande demais",
                            f"O limite é {MAX_BYTES // (1024 * 1024)} MB por imagem.")
        return ""
    return path


def _fit(size: QSize, box: QSize) -> QSize:
    """Reduz proporcionalmente para caber em box (nunca amplia)."""
    if size.width() <= box.width() and size.height() <= box.height():
        return size
    return size.scaled(box, Qt.AspectRatioMode.KeepAspectRatio)


def load_pixmap(path, box: QSize) -> QPixmap:
    """Decodifica já reduzido (mais rápido e leve que carregar o original inteiro)."""
    reader = QImageReader(str(path))
    reader.setAutoTransform(True)
    if reader.size().isValid():
        reader.setScaledSize(_fit(reader.size(), box))
    return QPixmap.fromImage(reader.read())


def make_movie(path, box: QSize) -> QMovie:
    movie = QMovie(str(path))
    size = QImageReader(str(path)).size()
    if size.isValid():
        movie.setScaledSize(_fit(size, box))
    return movie


class ViewerDialog(QDialog):
    """Imagem em tamanho maior. Clique ou Esc para fechar."""

    def __init__(self, path, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Imagem")
        area = QApplication.primaryScreen().availableGeometry()
        box = QSize(int(area.width() * 0.8), int(area.height() * 0.8))
        label = QLabel()
        label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        if Path(path).suffix == ".gif":
            self._movie = make_movie(path, box)
            label.setMovie(self._movie)
            self._movie.start()
        else:
            label.setPixmap(load_pixmap(path, box))
        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.addWidget(label)

    def mousePressEvent(self, event):
        self.accept()


class MediaLabel(QLabel):
    """Miniatura clicável. Mostra aviso até a imagem existir no disco."""

    def __init__(self, store, name, box=QSize(260, 200)):
        super().__init__()
        self.store, self.name, self.box = store, name, box
        self.loaded = False
        self.setObjectName("thumb")
        self.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)
        self.load()

    def load(self):
        path = self.store.media_path(self.name)
        if not path.exists():
            self.setText("Carregando imagem...")
            return
        if path.suffix == ".gif":
            self._movie = make_movie(path, self.box)
            self.setMovie(self._movie)
            self._movie.start()
        else:
            self.setPixmap(load_pixmap(path, self.box))
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.loaded = True

    def mousePressEvent(self, event):
        if self.loaded:
            ViewerDialog(self.store.media_path(self.name), self.window()).exec()
