"""Sistema de internacionalização (i18n) e persistência de configurações do LocalCall.

Idiomas suportados:
- pt_BR: Português (Brasil)
- en_US: English
- es_ES: Español
"""
import json
import sys
from pathlib import Path

SUPPORTED_LANGUAGES = {
    "pt_BR": "Português (Brasil)",
    "en_US": "English",
    "es_ES": "Español",
}

DEFAULT_LANGUAGE = "pt_BR"
_CURRENT_LANGUAGE = DEFAULT_LANGUAGE

TRANSLATIONS = {
    "pt_BR": {
        # App Geral
        "app_name": "LocalCall",
        "app_desc": "Comunicação de voz de baixa latência, tela e produtividade em rede local",
        "room_title": "LocalCall - {name}",
        "room_title_default": "LocalCall",
        "e2e_footer": "Criptografia ponta a ponta: AES-256-GCM + Argon2id",
        "e2e_chat_notice": "Suas mensagens, mídias, chamadas de voz e telas são protegidas com criptografia de ponta a ponta (AES-256-GCM / Argon2id).",
        "voice_active": "Voz: ativa ({port})",
        "voice_inactive": "Voz: inativa",
        "voice_missing_dll": "Voz: DLL ausente",
        "video_status": "Vídeo: {port}",
        "video_off": "Vídeo: off",
        "no_connection": "Sem conexão",
        "host_disconnected": "Host desconectado — a sessão foi encerrada",
        "e2e_verified": "Conexão Cifrada (E2EE: AES-256-GCM / Argon2id)",
        "e2e_verifying": "Verificando chave da sala (E2EE)...",
        "action_not_sent_no_conn": "Sem conexão: ação não enviada",
        "encrypting_image": "Cifrando e enviando imagem...",
        "image_size_err": "Imagem inválida ou maior que 10 MB",
        # Barra lateral
        "sidebar_connected": "CONECTADOS",
        "sidebar_voice_screen": "VOZ & TELA",
        "btn_connection": "Conexão...",
        "btn_share_screen": "Compartilhar Tela",
        "btn_stop_stream": "Parar Transmissão",
        "btn_mute_mic": "Mutar Mic",
        "btn_unmute_mic": "Mic Mutado",
        "btn_deafen": "Desativar Áudio",
        "btn_undeafen": "Áudio Desativado",
        "btn_noise_on": "Ruído: Ativo",
        "btn_noise_off": "Ruído: Desativado",
        "tooltip_noise_suppression": "Cancelamento de Ruído inteligente (estilo Discord) - elimina chiado, ventilador e ruídos de fundo",
        # Menu de Contexto de Participantes (Estilo Discord)
        "ctx_mute_peer": "Mutar para Mim",
        "ctx_user_volume": "Volume do Usuário: {pct}%",
        "ctx_reset_volume": "Restaurar Volume (100%)",
        "ctx_mute_screen_audio": "Mutar Áudio da Tela deste Usuário",
        "ctx_mod_header": "Moderação (Host)",
        "ctx_server_mute": "Silenciar para Todos (Server Mute)",
        "ctx_kick_peer": "Expulsar da Sala (Kick)",
        "msg_kicked_title": "Expulso da Sala",
        "msg_kicked_by_host": "Você foi expulso da sala pelo Host.",
        "msg_server_muted": "Seu microfone foi silenciado pelo Host.",
        "msg_peer_kicked": "{name} foi expulso da sala",
        "msg_peer_server_muted": "{name} foi silenciado para todos",
        "peer_badge_muted": "Mutado",
        "peer_status_streaming": "Transmitindo tela",
        "peer_status_voice": "Conectado na voz",
        # Diálogo de Conexão
        "conn_dialog_title": "LocalCall - Conexão Segura",
        "label_name": "Seu nome",
        "label_mode": "Modo",
        "mode_join": "Entrar em uma sala",
        "mode_host": "Hospedar sala",
        "label_radmin_ip": "Meu IP (Compartilhar)",
        "label_radmin_not_found": "Pronto para conexão direta (LAN / UPnP / IP Público)",
        "btn_copy_radmin": "Copiar meu Endereço",
        "label_address": "Endereço",
        "label_port": "Porta",
        "label_password": "Senha da sala",
        "placeholder_password": "Senha da sala (nunca trafega na rede)",
        "placeholder_address": "IP do host (LAN, IP Público:Porta ou VPN)",
        "btn_show_pw": "Mostrar",
        "btn_hide_pw": "Ocultar",
        "btn_gen_pw": "Gerar Forte",
        "btn_copy_pw": "Copiar Senha",
        "copied_pw": "Senha copiada para a área de transferência.",
        "copied_radmin": "Endereço ({ip}) copiado para a área de transferência.",
        "label_language": "Idioma",
        "weak_pw_title": "Senha Fraca",
        "weak_pw_msg": "{msg}\n\nDeseja continuar mesmo assim?",
        "host_limit_title": "Limite de Transmissões",
        "host_limit_msg": "Já existem 2 pessoas transmitindo tela na sala.\nAguarde uma transmissão encerrar para iniciar a sua.",
        # Abas
        "tab_screens": "Telas",
        "tab_screens_count": "Telas ({count})",
        "tab_chat": "Chat",
        "tab_tasks": "Tarefas",
        "tab_notes": "Notas",
        # Compartilhamento de Tela
        "screen_dlg_title": "Compartilhar Tela",
        "tab_monitors": "Telas",
        "tab_windows": "Janelas de Aplicativos",
        "hint_windows": "Selecione a janela do aplicativo ou jogo:",
        "btn_refresh_windows": "Atualizar Lista",
        "quality_header": "QUALIDADE DA TRANSMISSÃO (H.264 / BAIXA LATÊNCIA)",
        "label_resolution": "Resolução:",
        "label_framerate": "Taxa de Quadros:",
        "chk_share_sys_audio": "Compartilhar áudio do sistema (WASAPI Loopback — jogos e vídeos)",
        "target_bitrate": "Bitrate alvo: ~{mbps} Mbps (proporção original preservada)",
        "btn_cancel": "Cancelar",
        "btn_start_share": "Iniciar Apresentação",
        "monitor_title": "Tela {idx} ({aspect})",
        "sys_audio_on": "Áudio do Sistema: ON",
        "sys_audio_off": "Áudio do Sistema: OFF",
        "tooltip_sys_audio": "Capturar e transmitir o áudio do Windows (jogos, vídeos, música) sem eco",
        "no_active_streams": "Nenhuma transmissão ativa (limite: 2 telas simultâneas)",
        "active_streams": "Transmissão ativa ({count}/2): {names}",
        "waiting_video": "Aguardando vídeo de {title}...",
        "fullscreen_hint": "  •  ESC ou duplo clique para sair da tela cheia",
        "window_fullscreen_hint": "  •  Duplo clique para tela cheia",
        "local_presenter_title": "Você está apresentando sua tela",
        "local_presenter_desc": "Sua transmissão está visível em tempo real para os outros participantes da chamada.\nO LocalCall foi ocultado da transmissão para proteger sua privacidade e eliminar o efeito espelho.",
        "local_presenter_badge": "● Transmissão Ativa • 60 FPS • E2EE",
        # Chat
        "chat_placeholder": "Mensagem",
        "btn_image": "Imagem",
        "btn_send": "Enviar",
        "btn_reply": "Responder",
        "menu_reply": "Responder mensagem",
        "menu_delete": "Apagar mensagem",
        "replying_to": "Respondendo a {author}: {text}",
        "image_tag": "[Imagem]",
        # Tarefas e Pomodoro
        "pomo_focus": "POMODORO (FOCO)",
        "pomo_break": "POMODORO (PAUSA)",
        "btn_start": "Iniciar",
        "btn_pause": "Pausar",
        "btn_resume": "Retomar",
        "btn_reset": "Resetar",
        "task_placeholder": "Nova tarefa",
        "btn_add": "Adicionar",
        "hint_task_image": "Duplo clique em uma tarefa com imagem para ampliar",
        "hint_task_attached": "Imagem anexada à próxima tarefa: {name}",
        "btn_attach_selected": "Imagem na selecionada",
        "btn_remove_selected": "Remover selecionada",
        # Notas
        "btn_new_note": "Nova nota",
        "placeholder_note_title": "Título",
        "btn_save": "Salvar",
        "btn_delete": "Excluir",
        "last_edit": "Última edição: {author}",
        "untitled_note": "Sem título",
        # Instalador
        "installer_title": "Instalação - LocalCall",
        "installer_header_title": "Instalação do LocalCall",
        "installer_folder_label": "Pasta de Instalação:",
        "installer_browse": "Procurar...",
        "installer_chk_shortcut": "Criar atalho na Área de Trabalho",
        "installer_chk_launch": "Iniciar o LocalCall ao concluir a instalação",
        "installer_status_ready": "Clique em 'Instalar' para prosseguir.",
        "installer_btn_install": "Instalar",
        "installer_btn_finish": "Concluir",
        "installer_reading": "Lendo arquivos do pacote...",
        "installer_extracting": "Instalando: {name}",
        "installer_shortcuts": "Configurando atalhos...",
        "installer_done": "Instalação concluída com sucesso! Este instalador já pode ser apagado.",
        "installer_err_payload": "Erro: pacote de instalação (payload.zip) não encontrado.",
    },
    "en_US": {
        # General App
        "app_name": "LocalCall",
        "app_desc": "Low-latency voice communication, screen sharing, and local productivity",
        "room_title": "LocalCall - {name}",
        "room_title_default": "LocalCall",
        "e2e_footer": "End-to-end encryption: AES-256-GCM + Argon2id",
        "e2e_chat_notice": "Your messages, media, voice calls, and screens are protected with end-to-end encryption (AES-256-GCM / Argon2id).",
        "voice_active": "Voice: active ({port})",
        "voice_inactive": "Voice: inactive",
        "voice_missing_dll": "Voice: DLL missing",
        "video_status": "Video: {port}",
        "video_off": "Video: off",
        "no_connection": "No connection",
        "host_disconnected": "Host disconnected — session closed",
        "e2e_verified": "Encrypted Connection (E2EE: AES-256-GCM / Argon2id)",
        "e2e_verifying": "Verifying room key (E2EE)...",
        "action_not_sent_no_conn": "No connection: action not sent",
        "encrypting_image": "Encrypting and sending image...",
        "image_size_err": "Invalid image or size exceeds 10 MB",
        # Sidebar
        "sidebar_connected": "CONNECTED",
        "sidebar_voice_screen": "VOICE & SCREEN",
        "btn_connection": "Connection...",
        "btn_share_screen": "Share Screen",
        "btn_stop_stream": "Stop Stream",
        "btn_mute_mic": "Mute Mic",
        "btn_unmute_mic": "Mic Muted",
        "btn_deafen": "Deafen",
        "btn_undeafen": "Deafened",
        "btn_noise_on": "Noise: On",
        "btn_noise_off": "Noise: Off",
        "tooltip_noise_suppression": "Smart Noise Suppression (Discord-style) - eliminates hiss, fan noise, and background rumble",
        # Participant Context Menu (Discord-style)
        "ctx_mute_peer": "Mute for Me",
        "ctx_user_volume": "User Volume: {pct}%",
        "ctx_reset_volume": "Reset Volume (100%)",
        "ctx_mute_screen_audio": "Mute User's Screen Audio",
        "ctx_mod_header": "Moderation (Host)",
        "ctx_server_mute": "Server Mute (Mute for Everyone)",
        "ctx_kick_peer": "Kick from Room",
        "msg_kicked_title": "Kicked from Room",
        "msg_kicked_by_host": "You were kicked from the room by the Host.",
        "msg_server_muted": "Your microphone was muted by the Host.",
        "msg_peer_kicked": "{name} was kicked from the room",
        "msg_peer_server_muted": "{name} was server-muted",
        "peer_badge_muted": "Muted",
        "peer_status_streaming": "Streaming screen",
        "peer_status_voice": "Connected to voice",
        # Connect Dialog
        "conn_dialog_title": "LocalCall - Secure Connection",
        "label_name": "Your name",
        "label_mode": "Mode",
        "mode_join": "Join a room",
        "mode_host": "Host a room",
        "label_radmin_ip": "My IP (To Share)",
        "label_radmin_not_found": "Ready for direct connection (LAN / UPnP / Public IP)",
        "btn_copy_radmin": "Copy my Address",
        "label_address": "Address",
        "label_port": "Port",
        "label_password": "Room password",
        "placeholder_password": "Room password (never sent over network)",
        "placeholder_address": "Host IP (LAN, Public IP:Port or VPN)",
        "btn_show_pw": "Show",
        "btn_hide_pw": "Hide",
        "btn_gen_pw": "Generate Strong",
        "btn_copy_pw": "Copy Password",
        "copied_pw": "Password copied to clipboard.",
        "copied_radmin": "Address ({ip}) copied to clipboard.",
        "label_language": "Language",
        "weak_pw_title": "Weak Password",
        "weak_pw_msg": "{msg}\n\nDo you want to continue anyway?",
        "host_limit_title": "Stream Limit",
        "host_limit_msg": "2 people are already streaming screens in this room.\nPlease wait for a stream to finish.",
        # Tabs
        "tab_screens": "Screens",
        "tab_screens_count": "Screens ({count})",
        "tab_chat": "Chat",
        "tab_tasks": "Tasks",
        "tab_notes": "Notes",
        # Screen Share Dialog
        "screen_dlg_title": "Share Screen",
        "tab_monitors": "Screens",
        "tab_windows": "Application Windows",
        "hint_windows": "Select an application or game window:",
        "btn_refresh_windows": "Refresh List",
        "quality_header": "STREAM QUALITY (H.264 / LOW LATENCY)",
        "label_resolution": "Resolution:",
        "label_framerate": "Frame Rate:",
        "chk_share_sys_audio": "Share system audio (WASAPI Loopback — games and videos)",
        "target_bitrate": "Target bitrate: ~{mbps} Mbps (original aspect ratio preserved)",
        "btn_cancel": "Cancel",
        "btn_start_share": "Start Sharing",
        "monitor_title": "Screen {idx} ({aspect})",
        "sys_audio_on": "System Audio: ON",
        "sys_audio_off": "System Audio: OFF",
        "tooltip_sys_audio": "Capture and transmit Windows audio (games, videos, music) with no echo",
        "no_active_streams": "No active stream (limit: 2 simultaneous screens)",
        "active_streams": "Active stream ({count}/2): {names}",
        "waiting_video": "Waiting for video from {title}...",
        "fullscreen_hint": "  •  ESC or double-click to exit fullscreen",
        "window_fullscreen_hint": "  •  Double-click for fullscreen",
        "local_presenter_title": "You are sharing your screen",
        "local_presenter_desc": "Your screen is being shared in real-time with other participants.\nLocalCall is automatically hidden from capture to protect your privacy and eliminate the mirror effect.",
        "local_presenter_badge": "● Streaming Active • 60 FPS • E2EE",
        # Chat
        "chat_placeholder": "Message",
        "btn_image": "Image",
        "btn_send": "Send",
        "btn_reply": "Reply",
        "menu_reply": "Reply to message",
        "menu_delete": "Delete message",
        "replying_to": "Replying to {author}: {text}",
        "image_tag": "[Image]",
        # Tasks & Pomodoro
        "pomo_focus": "POMODORO (FOCUS)",
        "pomo_break": "POMODORO (BREAK)",
        "btn_start": "Start",
        "btn_pause": "Pause",
        "btn_resume": "Resume",
        "btn_reset": "Reset",
        "task_placeholder": "New task",
        "btn_add": "Add",
        "hint_task_image": "Double-click a task with an image to expand",
        "hint_task_attached": "Image attached to next task: {name}",
        "btn_attach_selected": "Image on selected",
        "btn_remove_selected": "Remove selected",
        # Notes
        "btn_new_note": "New note",
        "placeholder_note_title": "Title",
        "btn_save": "Save",
        "btn_delete": "Delete",
        "last_edit": "Last edited by: {author}",
        "untitled_note": "Untitled",
        # Installer
        "installer_title": "Setup - LocalCall",
        "installer_header_title": "LocalCall Setup",
        "installer_folder_label": "Installation Folder:",
        "installer_browse": "Browse...",
        "installer_chk_shortcut": "Create Desktop shortcut",
        "installer_chk_launch": "Launch LocalCall when setup completes",
        "installer_status_ready": "Click 'Install' to proceed.",
        "installer_btn_install": "Install",
        "installer_btn_finish": "Finish",
        "installer_reading": "Reading package files...",
        "installer_extracting": "Installing: {name}",
        "installer_shortcuts": "Setting up shortcuts...",
        "installer_done": "Installation completed successfully! You can now delete this installer.",
        "installer_err_payload": "Error: installation package (payload.zip) not found.",
    },
    "es_ES": {
        # App General
        "app_name": "LocalCall",
        "app_desc": "Comunicación de voz de baja latencia, pantalla y productividad en red local",
        "room_title": "LocalCall - {name}",
        "room_title_default": "LocalCall",
        "e2e_footer": "Cifrado de extremo a extremo: AES-256-GCM + Argon2id",
        "e2e_chat_notice": "Tus mensajes, archivos multimedia, llamadas de voz y pantallas están protegidos con cifrado de extremo a extremo (AES-256-GCM / Argon2id).",
        "voice_active": "Voz: activa ({port})",
        "voice_inactive": "Voz: inactiva",
        "voice_missing_dll": "Voz: DLL ausente",
        "video_status": "Vídeo: {port}",
        "video_off": "Vídeo: apagado",
        "no_connection": "Sin conexión",
        "host_disconnected": "Host desconectado — la sesión ha terminado",
        "e2e_verified": "Conexión Cifrada (E2EE: AES-256-GCM / Argon2id)",
        "e2e_verifying": "Verificando clave de la sala (E2EE)...",
        "action_not_sent_no_conn": "Sin conexión: acción no enviada",
        "encrypting_image": "Cifrando y enviando imagen...",
        "image_size_err": "Imagen inválida o mayor a 10 MB",
        # Barra lateral
        "sidebar_connected": "CONECTADOS",
        "sidebar_voice_screen": "VOZ Y PANTALLA",
        "btn_connection": "Conexión...",
        "btn_share_screen": "Compartir Pantalla",
        "btn_stop_stream": "Detener Pantalla",
        "btn_mute_mic": "Silenciar Mic",
        "btn_unmute_mic": "Mic Silenciado",
        "btn_deafen": "Desactivar Audio",
        "btn_undeafen": "Audio Desactivado",
        "btn_noise_on": "Ruido: Activo",
        "btn_noise_off": "Ruido: Desactivado",
        "tooltip_noise_suppression": "Cancelación de Ruido inteligente (estilo Discord) - elimina siseo, ventiladores y ruido de fondo",
        # Menú Contextual de Participantes (Estilo Discord)
        "ctx_mute_peer": "Silenciar para Mí",
        "ctx_user_volume": "Volumen del Usuario: {pct}%",
        "ctx_reset_volume": "Restablecer Volumen (100%)",
        "ctx_mute_screen_audio": "Silenciar Audio de Pantalla de este Usuario",
        "ctx_mod_header": "Moderación (Host)",
        "ctx_server_mute": "Silenciar para Todos (Server Mute)",
        "ctx_kick_peer": "Expulsar de la Sala (Kick)",
        "msg_kicked_title": "Expulsado de la Sala",
        "msg_kicked_by_host": "Fuiste expulsado de la sala por el Host.",
        "msg_server_muted": "Tu micrófono fue silenciado por el Host.",
        "msg_peer_kicked": "{name} fue expulsado de la sala",
        "msg_peer_server_muted": "{name} fue silenciado para todos",
        "peer_badge_muted": "Silenciado",
        "peer_status_streaming": "Transmitiendo pantalla",
        "peer_status_voice": "Conectado en voz",
        # Diálogo de Conexión
        "conn_dialog_title": "LocalCall - Conexión Segura",
        "label_name": "Tu nombre",
        "label_mode": "Modo",
        "mode_join": "Entrar a una sala",
        "mode_host": "Crear sala (Host)",
        "label_radmin_ip": "Mi IP (Para Compartir)",
        "label_radmin_not_found": "Listo para conexión directa (LAN / UPnP / IP Público)",
        "btn_copy_radmin": "Copiar mi Dirección",
        "label_address": "Dirección",
        "label_port": "Puerto",
        "label_password": "Clave de la sala",
        "placeholder_password": "Clave de la sala (nunca viaja por la red)",
        "placeholder_address": "IP del host (LAN, IP Público:Puerto o VPN)",
        "btn_show_pw": "Mostrar",
        "btn_hide_pw": "Ocultar",
        "btn_gen_pw": "Generar Fuerte",
        "btn_copy_pw": "Copiar Clave",
        "copied_pw": "Clave copiada al portapapeles.",
        "copied_radmin": "Dirección ({ip}) copiada al portapapeles.",
        "label_language": "Idioma",
        "weak_pw_title": "Clave Débil",
        "weak_pw_msg": "{msg}\n\n¿Deseas continuar de todos modos?",
        "host_limit_title": "Límite de Transmisiones",
        "host_limit_msg": "Ya hay 2 personas transmitiendo pantalla en la sala.\nEspera a que termine una transmisión.",
        # Pestañas
        "tab_screens": "Pantallas",
        "tab_screens_count": "Pantallas ({count})",
        "tab_chat": "Chat",
        "tab_tasks": "Tareas",
        "tab_notes": "Notas",
        # Compartir Pantalla
        "screen_dlg_title": "Compartir Pantalla",
        "tab_monitors": "Pantallas",
        "tab_windows": "Ventanas de Aplicaciones",
        "hint_windows": "Selecciona la ventana de la aplicación o juego:",
        "btn_refresh_windows": "Actualizar Lista",
        "quality_header": "CALIDAD DE TRANSMISIÓN (H.264 / BAJA LATENCIA)",
        "label_resolution": "Resolución:",
        "label_framerate": "Cuadros por segundo:",
        "chk_share_sys_audio": "Compartir audio del sistema (WASAPI Loopback — juegos y vídeos)",
        "target_bitrate": "Bitrate objetivo: ~{mbps} Mbps (proporción original conservada)",
        "btn_cancel": "Cancelar",
        "btn_start_share": "Comenzar Transmisión",
        "monitor_title": "Pantalla {idx} ({aspect})",
        "sys_audio_on": "Audio del Sistema: ON",
        "sys_audio_off": "Audio del Sistema: OFF",
        "tooltip_sys_audio": "Capturar y transmitir audio de Windows (juegos, vídeos, música) sin eco",
        "no_active_streams": "Ninguna transmisión activa (límite: 2 pantallas simultáneas)",
        "active_streams": "Transmisión activa ({count}/2): {names}",
        "waiting_video": "Esperando vídeo de {title}...",
        "fullscreen_hint": "  •  ESC o doble clic para salir de pantalla completa",
        "window_fullscreen_hint": "  •  Doble clic para pantalla completa",
        "local_presenter_title": "Estás compartiendo tu pantalla",
        "local_presenter_desc": "Tu pantalla se está transmitiendo en tiempo real a los demás participantes.\nLocalCall se oculta automáticamente de la captura para proteger tu privacidad y evitar el efecto espejo.",
        "local_presenter_badge": "● Transmisión Activa • 60 FPS • E2EE",
        # Chat
        "chat_placeholder": "Mensaje",
        "btn_image": "Imagen",
        "btn_send": "Enviar",
        "btn_reply": "Responder",
        "menu_reply": "Responder mensaje",
        "menu_delete": "Eliminar mensaje",
        "replying_to": "Respondiendo a {author}: {text}",
        "image_tag": "[Imagen]",
        # Tareas y Pomodoro
        "pomo_focus": "POMODORO (ENFOQUE)",
        "pomo_break": "POMODORO (PAUSA)",
        "btn_start": "Iniciar",
        "btn_pause": "Pausar",
        "btn_resume": "Reanudar",
        "btn_reset": "Reiniciar",
        "task_placeholder": "Nueva tarea",
        "btn_add": "Añadir",
        "hint_task_image": "Doble clic en una tarea con imagen para ampliar",
        "hint_task_attached": "Imagen adjunta a la siguiente tarea: {name}",
        "btn_attach_selected": "Imagen en la seleccionada",
        "btn_remove_selected": "Eliminar seleccionada",
        # Notas
        "btn_new_note": "Nueva nota",
        "placeholder_note_title": "Título",
        "btn_save": "Guardar",
        "btn_delete": "Eliminar",
        "last_edit": "Última edición por: {author}",
        "untitled_note": "Sin título",
        # Instalador
        "installer_title": "Instalación - LocalCall",
        "installer_header_title": "Instalación de LocalCall",
        "installer_folder_label": "Carpeta de Instalación:",
        "installer_browse": "Explorar...",
        "installer_chk_shortcut": "Crear acceso directo en el Escritorio",
        "installer_chk_launch": "Iniciar LocalCall al finalizar la instalación",
        "installer_status_ready": "Haz clic en 'Instalar' para continuar.",
        "installer_btn_install": "Instalar",
        "installer_btn_finish": "Finalizar",
        "installer_reading": "Leyendo archivos del paquete...",
        "installer_extracting": "Instalando: {name}",
        "installer_shortcuts": "Configurando accesos directos...",
        "installer_done": "¡Instalación completada con éxito! Ya puedes borrar este instalador.",
        "installer_err_payload": "Error: no se encontró el paquete de instalación (payload.zip).",
    },
}


def _get_config_path() -> Path:
    if getattr(sys, "frozen", False):
        base = Path(sys.executable).resolve().parent
    else:
        base = Path(__file__).resolve().parent.parent
    return base / "config.json"


def load_config() -> dict:
    p = _get_config_path()
    if p.is_file():
        try:
            return json.loads(p.read_text(encoding="utf-8"))
        except Exception:
            pass
    return {"language": DEFAULT_LANGUAGE}


def save_config(cfg: dict):
    p = _get_config_path()
    try:
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(cfg, indent=2, ensure_ascii=False), encoding="utf-8")
    except Exception:
        pass


def init_language():
    global _CURRENT_LANGUAGE
    cfg = load_config()
    lang = cfg.get("language", DEFAULT_LANGUAGE)
    if lang in SUPPORTED_LANGUAGES:
        _CURRENT_LANGUAGE = lang
    else:
        _CURRENT_LANGUAGE = DEFAULT_LANGUAGE


def set_language(lang_code: str):
    global _CURRENT_LANGUAGE
    if lang_code in SUPPORTED_LANGUAGES:
        _CURRENT_LANGUAGE = lang_code
        cfg = load_config()
        cfg["language"] = lang_code
        save_config(cfg)


def get_language() -> str:
    return _CURRENT_LANGUAGE


def t(key: str, **kwargs) -> str:
    """Retorna o texto traduzido para o idioma ativo com fallback seguro."""
    lang_dict = TRANSLATIONS.get(_CURRENT_LANGUAGE, TRANSLATIONS[DEFAULT_LANGUAGE])
    template = lang_dict.get(key)
    if template is None:
        template = TRANSLATIONS[DEFAULT_LANGUAGE].get(key, key)
    if kwargs:
        try:
            return template.format(**kwargs)
        except Exception:
            return template
    return template


# Inicializa idioma padrão ao importar
init_language()
