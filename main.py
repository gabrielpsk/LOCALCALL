import sys
from pathlib import Path

from PyQt6.QtGui import QIcon
from PyQt6.QtWidgets import QApplication

from network.radmin import ensure_radmin_launched
from ui.i18n import init_language
from ui.main_window import MainWindow


def _resolve_resource(rel_path: str) -> Path:
    if getattr(sys, "frozen", False):
        meipass = getattr(sys, "_MEIPASS", None)
        if meipass:
            return Path(meipass) / rel_path
        return Path(sys.executable).resolve().parent / rel_path
    return Path(__file__).resolve().parent / rel_path


def main():
    init_language()
    app = QApplication(sys.argv)
    app.setStyle("Fusion")

    # Ícone minimalista do aplicativo
    icon_path = _resolve_resource("ui/icon.png")
    if icon_path.is_file():
        app.setWindowIcon(QIcon(str(icon_path)))

    # Verifica/inicia Radmin VPN automaticamente na inicialização
    radmin_ok, radmin_msg = ensure_radmin_launched()
    radmin_notice = "" if radmin_ok else radmin_msg

    # QSS em arquivo separado (compatível com código-fonte e executável PyInstaller)
    qss = _resolve_resource("ui/style.qss")
    if qss.is_file():
        app.setStyleSheet(qss.read_text(encoding="utf-8"))

    window = MainWindow(radmin_notice=radmin_notice)
    window.show()
    window.connect_dialog()   # pede nome e modo logo ao abrir
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
