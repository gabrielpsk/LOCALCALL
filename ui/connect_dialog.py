"""Diálogo de conexão: hospedar ou entrar em uma sala com senha E2EE e detecção de IP (VPN / ZeroTier / Radmin / LAN)."""
import platform

from PyQt6.QtWidgets import (QApplication, QComboBox, QDialog, QDialogButtonBox,
                             QFormLayout, QHBoxLayout, QLabel, QLineEdit,
                             QMessageBox, QPushButton, QSpinBox, QVBoxLayout)

from network.crypto import generate_strong_password, is_password_strong
from network.protocol import DEFAULT_PORT, list_network_ips
from network.radmin import detect_radmin_ip

_IS_LINUX = platform.system() == "Linux"


class ConnectDialog(QDialog):
    def __init__(self, parent=None, last_name="", last_password="", radmin_notice=""):
        super().__init__(parent)
        self.setWindowTitle("Conexão Segura (E2EE)")
        self.radmin_ip = detect_radmin_ip()

        self.name = QLineEdit(last_name)
        self.mode = QComboBox()
        self.mode.addItems(["Entrar em uma sala", "Hospedar sala"])
        self.address = QLineEdit()
        if _IS_LINUX:
            self.address.setPlaceholderText("IP do host (ZeroTier, Radmin VPN, tun0 ou LAN)")
        else:
            self.address.setPlaceholderText("IP do host (Radmin 26.x.x.x, ZeroTier ou LAN)")
        self.port = QSpinBox()
        self.port.setRange(1024, 65535)
        self.port.setValue(DEFAULT_PORT)

        # Seletor/Exibição automática de IPs (ZeroTier zt..., VPN tun0, Radmin 26.x.x.x, eth0/wlan0) + botão "Copiar meu IP"
        self.ip_combo = QComboBox()
        self.radmin_label = QLabel()
        self.radmin_label.setObjectName("hint")
        self.copy_ip_btn = QPushButton("Copiar meu IP")
        self.copy_ip_btn.clicked.connect(self._copy_radmin_ip)

        ip_row = QHBoxLayout()
        ip_row.setContentsMargins(0, 0, 0, 0)
        ip_row.setSpacing(8)
        ip_row.addWidget(self.ip_combo, 1)
        ip_row.addWidget(self.radmin_label, 1)
        ip_row.addWidget(self.copy_ip_btn)
        self._refresh_ips()

        # Campo de senha mascarado + botões Mostrar, Gerar e Copiar
        self.password = QLineEdit(last_password or generate_strong_password())
        self.password.setEchoMode(QLineEdit.EchoMode.Password)
        self.password.setPlaceholderText("Senha da sala (nunca trafega na rede)")
        self.password.textChanged.connect(self._update_pw_hint)

        self.toggle_pw_btn = QPushButton("Mostrar")
        self.toggle_pw_btn.setCheckable(True)
        self.toggle_pw_btn.toggled.connect(self._toggle_echo)

        self.gen_pw_btn = QPushButton("Gerar Forte")
        self.gen_pw_btn.clicked.connect(self._generate_pw)

        self.copy_pw_btn = QPushButton("Copiar Senha")
        self.copy_pw_btn.clicked.connect(self._copy_pw)

        pw_actions = QHBoxLayout()
        pw_actions.setContentsMargins(0, 0, 0, 0)
        pw_actions.setSpacing(6)
        pw_actions.addWidget(self.toggle_pw_btn)
        pw_actions.addWidget(self.gen_pw_btn)
        pw_actions.addWidget(self.copy_pw_btn)

        self.pw_hint = QLabel()
        self.pw_hint.setObjectName("hint")
        self.pw_hint.setWordWrap(True)

        ip_row_title = "Meu IP (VPN/LAN)" if _IS_LINUX else "Meu IP"
        form = QFormLayout()
        form.addRow("Seu nome", self.name)
        form.addRow("Modo", self.mode)
        form.addRow(ip_row_title, ip_row)
        form.addRow("Endereço", self.address)
        form.addRow("Porta", self.port)
        form.addRow("Senha da sala", self.password)
        form.addRow("", pw_actions)
        form.addRow("", self.pw_hint)

        if radmin_notice:
            notice_lbl = QLabel(radmin_notice)
            notice_lbl.setObjectName("hint")
            notice_lbl.setWordWrap(True)
            form.addRow("", notice_lbl)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self._validate)
        buttons.rejected.connect(self.reject)

        layout = QVBoxLayout(self)
        layout.addLayout(form)
        layout.addWidget(buttons)
        self.mode.currentIndexChanged.connect(self._on_mode_changed)
        self._update_pw_hint()
        self.setMinimumWidth(460)

    def _refresh_ips(self):
        ips = list_network_ips()
        self.ip_combo.clear()
        for ip, desc in ips:
            self.ip_combo.addItem(desc, ip)

        if len(ips) > 1:
            self.ip_combo.show()
            self.radmin_label.hide()
            self.radmin_ip = ips[0][0]
            self.copy_ip_btn.setEnabled(True)
        elif len(ips) == 1:
            self.ip_combo.hide()
            self.radmin_label.show()
            self.radmin_ip = ips[0][0]
            self.radmin_label.setText(ips[0][1])
            self.copy_ip_btn.setEnabled(True)
        else:
            self.ip_combo.hide()
            self.radmin_label.show()
            self.radmin_ip = None
            self.radmin_label.setText("Nenhuma interface VPN/LAN ativa detectada")
            self.copy_ip_btn.setEnabled(False)

    def _on_mode_changed(self, idx: int):
        is_join = idx == 0
        self.address.setEnabled(is_join)
        self._refresh_ips()
        if not is_join and not self.password.text().strip():
            self._generate_pw()

    def _copy_radmin_ip(self):
        if self.ip_combo.isVisible() and self.ip_combo.currentData():
            selected_ip = str(self.ip_combo.currentData())
        else:
            selected_ip = detect_radmin_ip() or self.radmin_ip or ""
        if selected_ip:
            self.radmin_ip = selected_ip
            QApplication.clipboard().setText(selected_ip)
            self.pw_hint.setText(f"IP ({selected_ip}) copiado para a área de transferência.")

    def _toggle_echo(self, checked: bool):
        self.password.setEchoMode(
            QLineEdit.EchoMode.Normal if checked else QLineEdit.EchoMode.Password
        )
        self.toggle_pw_btn.setText("Ocultar" if checked else "Mostrar")

    def _generate_pw(self):
        self.password.setText(generate_strong_password())

    def _copy_pw(self):
        QApplication.clipboard().setText(self.password.text().strip())
        self.pw_hint.setText("Senha copiada para a área de transferência.")

    def _update_pw_hint(self):
        ok, msg = is_password_strong(self.password.text())
        self.pw_hint.setText(msg)

    def _validate(self):
        if not self.name.text().strip():
            self.name.setFocus()
            return
        if self.mode.currentIndex() == 0 and not self.address.text().strip():
            self.address.setFocus()
            return
        pw = self.password.text().strip()
        if not pw:
            self.password.setFocus()
            return

        strong, msg = is_password_strong(pw)
        if not strong:
            ans = QMessageBox.warning(
                self,
                "Senha Fraca",
                f"{msg}\n\nDeseja continuar mesmo assim?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No,
            )
            if ans != QMessageBox.StandardButton.Yes:
                self.password.setFocus()
                return
        self.accept()

    def values(self):
        return {
            "name": self.name.text().strip(),
            "host_mode": self.mode.currentIndex() == 1,
            "address": self.address.text().strip(),
            "port": self.port.value(),
            "password": self.password.text().strip(),
        }
