"""Instalador Profissional do LocalCall com UI Clara (Light Mode),

seleção de idioma internacional (Português, English, Español),
criação de atalho na Área de Trabalho e inicialização automática.
Pode ser apagado após a instalação.
"""
import json
import os
import shutil
import subprocess
import sys
import threading
import time
import zipfile
from pathlib import Path

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtGui import QFont, QIcon
from PyQt6.QtWidgets import (QApplication, QCheckBox, QComboBox, QDialog,
                             QFileDialog, QFrame, QHBoxLayout, QLabel,
                             QLineEdit, QProgressBar, QPushButton,
                             QVBoxLayout, QWidget)

from ui.i18n import SUPPORTED_LANGUAGES, get_language, set_language, t

DEFAULT_APP_NAME = "LocalCall"


def _get_payload_zip() -> Path | None:
    """Localiza o arquivo payload.zip (embutido pelo PyInstaller ou na pasta local)."""
    if getattr(sys, "frozen", False):
        meipass = getattr(sys, "_MEIPASS", None)
        if meipass:
            cand = Path(meipass) / "payload.zip"
            if cand.is_file():
                return cand
        cand = Path(sys.executable).resolve().parent / "payload.zip"
        if cand.is_file():
            return cand
    cand = Path(__file__).resolve().parent / "payload.zip"
    if cand.is_file():
        return cand
    return None


def _get_default_install_dir() -> Path:
    local_app_data = os.environ.get("LOCALAPPDATA")
    if local_app_data:
        return Path(local_app_data) / "Programs" / "LocalCall"
    return Path.home() / "LocalCall"


def _create_desktop_shortcut(target_exe: Path, app_name: str = DEFAULT_APP_NAME) -> bool:
    """Cria atalho (.lnk) na Área de Trabalho do Windows usando WScript.Shell nativo."""
    desktop = Path(os.environ.get("USERPROFILE", str(Path.home()))) / "Desktop"
    if not desktop.is_dir():
        desktop_pt = Path.home() / "Área de Trabalho"
        if desktop_pt.is_dir():
            desktop = desktop_pt
        else:
            onedrive_dt = Path(os.environ.get("USERPROFILE", "")) / "OneDrive" / "Área de Trabalho"
            if onedrive_dt.is_dir():
                desktop = onedrive_dt
            else:
                onedrive_en = Path(os.environ.get("USERPROFILE", "")) / "OneDrive" / "Desktop"
                if onedrive_en.is_dir():
                    desktop = onedrive_en

    shortcut_path = desktop / f"{app_name}.lnk"
    ico_candidate = target_exe.parent / "icon.ico"
    icon_loc = str(ico_candidate) if ico_candidate.is_file() else f"{str(target_exe)},0"

    ps_script = f"""
    $ws = New-Object -ComObject WScript.Shell
    $s = $ws.CreateShortcut('{str(shortcut_path)}')
    $s.TargetPath = '{str(target_exe)}'
    $s.WorkingDirectory = '{str(target_exe.parent)}'
    $s.IconLocation = '{icon_loc}'
    $s.Description = 'LocalCall - P2P Voice, Video & Productivity'
    $s.Save()
    """
    try:
        subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", ps_script],
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000),
            check=True,
            timeout=5,
        )
        return True
    except Exception:
        return False


LIGHT_STYLE = """
* {
    font-family: "Segoe UI", sans-serif;
    font-size: 9.5pt;
    color: #1f2937;
}

QDialog, QWidget {
    background-color: #f9fafb;
}

/* Painel de Cabeçalho Superior */
QFrame#headerBanner {
    background-color: #ffffff;
    border-bottom: 1px solid #e5e7eb;
}

QLabel#titleLabel {
    font-size: 13pt;
    font-weight: 700;
    color: #111827;
}

QLabel#subTitleLabel {
    font-size: 9pt;
    color: #4b5563;
}

/* Campos e Combos */
QLineEdit, QComboBox {
    background-color: #ffffff;
    border: 1px solid #d1d5db;
    border-radius: 4px;
    padding: 6px 10px;
    color: #111827;
}
QLineEdit:focus, QComboBox:focus {
    border: 1.5px solid #2563eb;
}

/* Botões */
QPushButton {
    background-color: #ffffff;
    border: 1px solid #d1d5db;
    border-radius: 4px;
    padding: 6px 16px;
    font-weight: 500;
    color: #374151;
}
QPushButton:hover {
    background-color: #f3f4f6;
    border-color: #9ca3af;
}
QPushButton:pressed {
    background-color: #e5e7eb;
}

QPushButton[primary="true"] {
    background-color: #2563eb;
    border: 1px solid #1d4ed8;
    color: #ffffff;
    font-weight: 600;
}
QPushButton[primary="true"]:hover {
    background-color: #1d4ed8;
}
QPushButton[primary="true"]:pressed {
    background-color: #1e40af;
}
QPushButton[primary="true"]:disabled {
    background-color: #93c5fd;
    border-color: #93c5fd;
    color: #ffffff;
}

/* Barra de Progresso */
QProgressBar {
    background-color: #e5e7eb;
    border: 1px solid #d1d5db;
    border-radius: 4px;
    text-align: center;
    color: #111827;
    font-size: 8.5pt;
    height: 18px;
}
QProgressBar::chunk {
    background-color: #2563eb;
    border-radius: 3px;
}

/* Checkbox */
QCheckBox {
    spacing: 8px;
    color: #374151;
}
QCheckBox::indicator {
    width: 16px;
    height: 16px;
    border: 1px solid #9ca3af;
    border-radius: 3px;
    background-color: #ffffff;
}
QCheckBox::indicator:checked {
    background-color: #2563eb;
    border-color: #2563eb;
    image: none;
}

/* Rodapé */
QFrame#footerFrame {
    background-color: #f3f4f6;
    border-top: 1px solid #e5e7eb;
}
"""


class InstallerWindow(QDialog):
    progress_signal = pyqtSignal(int, str)
    finished_signal = pyqtSignal(bool, str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFixedSize(580, 430)
        self.setStyleSheet(LIGHT_STYLE)

        self.payload_zip = _get_payload_zip()
        self.install_dir = _get_default_install_dir()
        self.is_installed = False

        self._build_ui()
        self.progress_signal.connect(self._on_progress)
        self.finished_signal.connect(self._on_finished)
        self._retranslate()

    def _build_ui(self):
        # Ícone da janela do instalador
        icon_path = Path(__file__).resolve().parent / "ui" / "icon.png"
        if not icon_path.is_file():
            meipass = getattr(sys, "_MEIPASS", None)
            if meipass:
                icon_path = Path(meipass) / "ui" / "icon.png"
        if icon_path.is_file():
            self.setWindowIcon(QIcon(str(icon_path)))

        # 1. Cabeçalho com ícone, títulos e seletor de idioma
        header = QFrame()
        header.setObjectName("headerBanner")
        header_lay = QHBoxLayout(header)
        header_lay.setContentsMargins(20, 14, 20, 14)
        header_lay.setSpacing(14)

        if icon_path.is_file():
            icon_img = QLabel()
            pix = QIcon(str(icon_path)).pixmap(48, 48)
            icon_img.setPixmap(pix)
            header_lay.addWidget(icon_img)

        text_col = QVBoxLayout()
        text_col.setSpacing(3)
        self.title_lbl = QLabel()
        self.title_lbl.setObjectName("titleLabel")
        self.subtitle_lbl = QLabel()
        self.subtitle_lbl.setObjectName("subTitleLabel")
        text_col.addWidget(self.title_lbl)
        text_col.addWidget(self.subtitle_lbl)
        header_lay.addLayout(text_col, 1)

        # Seletor de Idioma no topo direito do instalador
        self.lang_combo = QComboBox()
        cur_lang = get_language()
        cur_idx = 0
        for idx, (code, label) in enumerate(SUPPORTED_LANGUAGES.items()):
            self.lang_combo.addItem(label, code)
            if code == cur_lang:
                cur_idx = idx
        self.lang_combo.setCurrentIndex(cur_idx)
        self.lang_combo.currentIndexChanged.connect(self._on_lang_changed)
        header_lay.addWidget(self.lang_combo, 0, Qt.AlignmentFlag.AlignTop)

        # 2. Corpo Central (Body)
        body = QWidget()
        body_lay = QVBoxLayout(body)
        body_lay.setContentsMargins(24, 18, 24, 16)
        body_lay.setSpacing(14)

        self.folder_label = QLabel()
        self.folder_label.setStyleSheet("font-weight: 600; color: #374151;")

        folder_row = QHBoxLayout()
        folder_row.setSpacing(8)
        self.dir_input = QLineEdit(str(self.install_dir))
        self.dir_input.textChanged.connect(self._on_dir_changed)
        self.browse_btn = QPushButton()
        self.browse_btn.clicked.connect(self._browse_dir)
        folder_row.addWidget(self.dir_input, 1)
        folder_row.addWidget(self.browse_btn)

        # Opções
        options_box = QVBoxLayout()
        options_box.setSpacing(8)
        self.shortcut_chk = QCheckBox()
        self.shortcut_chk.setChecked(True)
        self.launch_chk = QCheckBox()
        self.launch_chk.setChecked(True)
        options_box.addWidget(self.shortcut_chk)
        options_box.addWidget(self.launch_chk)

        # Barra de Progresso e Status
        self.progress_bar = QProgressBar()
        self.progress_bar.setRange(0, 100)
        self.progress_bar.setValue(0)
        self.progress_bar.hide()

        self.status_lbl = QLabel()
        self.status_lbl.setStyleSheet("color: #6b7280; font-size: 8.5pt;")

        body_lay.addWidget(self.folder_label)
        body_lay.addLayout(folder_row)
        body_lay.addLayout(options_box)
        body_lay.addSpacing(4)
        body_lay.addWidget(self.progress_bar)
        body_lay.addWidget(self.status_lbl)
        body_lay.addStretch()

        # 3. Rodapé com Botões de Ação
        footer = QFrame()
        footer.setObjectName("footerFrame")
        footer_lay = QHBoxLayout(footer)
        footer_lay.setContentsMargins(24, 12, 24, 12)
        footer_lay.setSpacing(10)
        footer_lay.addStretch()

        self.cancel_btn = QPushButton()
        self.cancel_btn.clicked.connect(self.close)

        self.install_btn = QPushButton()
        self.install_btn.setProperty("primary", True)
        self.install_btn.clicked.connect(self._start_install)

        footer_lay.addWidget(self.cancel_btn)
        footer_lay.addWidget(self.install_btn)

        # Layout Principal
        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)
        root.addWidget(header)
        root.addWidget(body, 1)
        root.addWidget(footer)

    def _retranslate(self):
        self.setWindowTitle(t("installer_title"))
        self.title_lbl.setText(t("installer_header_title"))
        self.subtitle_lbl.setText(t("app_desc"))
        self.folder_label.setText(t("installer_folder_label"))
        self.browse_btn.setText(t("installer_browse"))
        self.shortcut_chk.setText(t("installer_chk_shortcut"))
        self.launch_chk.setText(t("installer_chk_launch"))
        if not self.is_installed:
            self.status_lbl.setText(t("installer_status_ready"))
            self.install_btn.setText(t("installer_btn_install"))
            self.cancel_btn.setText(t("btn_cancel"))
        else:
            self.status_lbl.setText(t("installer_done"))
            self.install_btn.setText(t("installer_btn_finish"))

    def _on_lang_changed(self, idx: int):
        code = self.lang_combo.itemData(idx)
        if code:
            set_language(code)
            self._retranslate()

    def _browse_dir(self):
        chosen = QFileDialog.getExistingDirectory(self, t("installer_browse"), str(self.install_dir))
        if chosen:
            p = Path(chosen)
            if p.name != "LocalCall":
                p = p / "LocalCall"
            self.install_dir = p
            self.dir_input.setText(str(p))

    def _on_dir_changed(self, text: str):
        t_str = text.strip()
        if t_str:
            self.install_dir = Path(t_str)

    def _start_install(self):
        if self.is_installed:
            target_exe = self.install_dir / "LocalCall.exe"
            if self.launch_chk.isChecked() and target_exe.is_file():
                subprocess.Popen(
                    [str(target_exe)],
                    cwd=str(self.install_dir),
                    creationflags=getattr(subprocess, "DETACHED_PROCESS", 0x00000008),
                )
            self.accept()
            return

        if not self.payload_zip or not self.payload_zip.is_file():
            self.status_lbl.setText(t("installer_err_payload"))
            self.status_lbl.setStyleSheet("color: #dc2626; font-weight: 600;")
            return

        self.lang_combo.setEnabled(False)
        self.dir_input.setEnabled(False)
        self.browse_btn.setEnabled(False)
        self.shortcut_chk.setEnabled(False)
        self.cancel_btn.setEnabled(False)
        self.install_btn.setEnabled(False)
        self.progress_bar.show()
        self.progress_bar.setValue(5)
        self.status_lbl.setText(t("installer_reading"))
        self.status_lbl.setStyleSheet("color: #2563eb;")

        threading.Thread(target=self._worker_install, daemon=True).start()

    def _worker_install(self):
        try:
            dest = self.install_dir
            dest.mkdir(parents=True, exist_ok=True)

            self.progress_signal.emit(15, t("installer_reading"))
            time.sleep(0.15)

            with zipfile.ZipFile(self.payload_zip, "r") as zf:
                members = zf.infolist()
                total = len(members)
                for idx, member in enumerate(members):
                    zf.extract(member, dest)
                    pct = 15 + int((idx + 1) / max(1, total) * 70)
                    if idx % 10 == 0 or idx == total - 1:
                        self.progress_signal.emit(pct, t("installer_extracting", name=Path(member.filename).name))

            target_exe = dest / "LocalCall.exe"
            if not target_exe.is_file():
                nested = dest / "LocalCall" / "LocalCall.exe"
                if nested.is_file():
                    for item in (dest / "LocalCall").iterdir():
                        shutil.move(str(item), str(dest / item.name))
                    try:
                        (dest / "LocalCall").rmdir()
                    except OSError:
                        pass

            # Salva o arquivo config.json com o idioma escolhido no instalador
            chosen_language = get_language()
            cfg_path = dest / "config.json"
            cfg_path.write_text(
                json.dumps({"language": chosen_language}, indent=2, ensure_ascii=False),
                encoding="utf-8",
            )

            self.progress_signal.emit(90, t("installer_shortcuts"))

            # Cria atalho na Área de Trabalho se marcado
            if self.shortcut_chk.isChecked():
                _create_desktop_shortcut(dest / "LocalCall.exe")

            # Cria script de desinstalação simples na pasta do app
            uninstaller_script = dest / "desinstalar.bat"
            uninstaller_script.write_text(
                f"@echo off\r\n"
                f"echo Desinstalando LocalCall...\r\n"
                f"taskkill /F /IM LocalCall.exe >nul 2>nul\r\n"
                f"timeout /t 1 >nul\r\n"
                f'del /Q "%USERPROFILE%\\Desktop\\LocalCall.lnk" >nul 2>nul\r\n'
                f'del /Q "%USERPROFILE%\\OneDrive\\Área de Trabalho\\LocalCall.lnk" >nul 2>nul\r\n'
                f'del /Q "%USERPROFILE%\\Área de Trabalho\\LocalCall.lnk" >nul 2>nul\r\n'
                f'rd /S /Q "{str(dest)}" >nul 2>nul\r\n'
                f"echo Desinstalacao concluida.\r\n"
                f"pause\r\n",
                encoding="latin-1",
            )

            self.progress_signal.emit(100, t("installer_done"))
            time.sleep(0.2)
            self.finished_signal.emit(True, t("installer_done"))

        except Exception as e:
            self.finished_signal.emit(False, f"Falha na instalação: {str(e)}")

    def _on_progress(self, percent: int, msg: str):
        self.progress_bar.setValue(percent)
        self.status_lbl.setText(msg)

    def _on_finished(self, success: bool, msg: str):
        if success:
            self.is_installed = True
            self.status_lbl.setText(msg)
            self.status_lbl.setStyleSheet("color: #059669; font-weight: 600;")
            self.cancel_btn.hide()
            self.install_btn.setText(t("installer_btn_finish"))
            self.install_btn.setEnabled(True)
            self.install_btn.setFocus()
        else:
            self.status_lbl.setText(msg)
            self.status_lbl.setStyleSheet("color: #dc2626; font-weight: 600;")
            self.cancel_btn.setEnabled(True)
            self.install_btn.setEnabled(True)


def main():
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    dlg = InstallerWindow()
    dlg.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
