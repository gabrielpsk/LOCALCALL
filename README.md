# 🚀 LocalCall — P2P Voice Call & 60 FPS Screen Share

> **Chamadas de voz ultrarrápidas, streaming de tela a 60 FPS e ferramentas de produtividade para você e seus amigos.**  
> Sem servidores corporativos no meio do caminho, sem telemetria, sem comer metade da sua memória RAM. Direto na rede local ou via VPN virtual (Radmin VPN / ZeroTier).

🌐 **Site Oficial e Downloads:** [https://localcall.netlify.app/](https://localcall.netlify.app/)  
📖 **Guia de Conexão Radmin VPN:** [https://localcall.netlify.app/tutorial-windows.html](https://localcall.netlify.app/tutorial-windows.html)  
🐧 **Guia Linux / Kali / ZeroTier:** [https://localcall.netlify.app/tutorial-linux.html](https://localcall.netlify.app/tutorial-linux.html)

[![Website](https://img.shields.io/badge/Website-localcall.netlify.app-6366f1?style=for-the-badge&logo=netlify&logoColor=white)](https://localcall.netlify.app/)
[![Python 3.10+](https://img.shields.io/badge/Python-3.10+-3776AB?style=for-the-badge&logo=python&logoColor=white)](https://python.org)
[![C Nativo](https://img.shields.io/badge/C_Engine-DirectX_%2F_WASAPI_%2F_X11-00599C?style=for-the-badge&logo=c&logoColor=white)](#como-compilar-do-código-fonte)
[![DirectX DXGI](https://img.shields.io/badge/Capture-DXGI_%26_X11_60FPS-0078D7?style=for-the-badge&logo=windows&logoColor=white)](#-apresentação-de-tela-a-60-fps-sem-engasgo)
[![Segurança AES-256-GCM](https://img.shields.io/badge/Crypto-AES--256--GCM_%7C_Argon2id-critical?style=for-the-badge&logo=lock)](#-criptografia-ponta-a-ponta-de-verdade-e2ee)
[![Licença MIT](https://img.shields.io/badge/License-MIT-success?style=for-the-badge)](LICENSE)

---

## 💡 Por que criamos esse app?

Quem nunca passou raiva com o Discord ou Teams travando o PC no meio de uma partida ranqueada ou gastando 1.5 GB de RAM só para conversar com três amigos?

Além disso, estamos cansados de:
- **Depender de nuvens de terceiros:** se o servidor deles cai, seu grupo fica no escuro.
- **Transmissão borrada:** streams que viram um borrão de pixels quando a ação esquenta no jogo.
- **Políticas abusivas de privacidade:** dados de voz, tela e texto sendo coletados e analisados em data centers remotos.

Decidimos criar nossa própria solução do zero. A arquitetura divide o melhor dos dois mundos:
1. **Interface amigável e produtiva em Python (PyQt6):** moderna, fácil de mexer, com chat, quadro de tarefas e pomodoro.
2. **Motor em C puro nativo:** processamento pesado de áudio (WASAPI com detecção de voz VAD) e captura de vídeo direto pela GPU (DXGI + encoder H.264 por hardware via Media Foundation) com criptografia acelerada por instruções de hardware.

O resultado? Consumo mínimo de hardware, latência de rede quase imperceptível na LAN e controle 100% nas mãos de quem está usando.

---

## ⚡ Principais Funcionalidades

### 🖥️ Apresentação de Tela a 60 FPS (Sem Engasgo)
Em vez de fazer captura de tela lenta puxando pixels para a CPU, usamos a **DXGI Desktop Duplication API** do DirectX 11 integrada ao codificador **H.264 via Media Foundation**.
- O quadro é capturado direto da memória de vídeo (VRAM) e codificado por hardware na GPU.
- O fluxo de vídeo sai fragmentado em pacotes UDP criptografados direto para os amigos da sala.
- Ideal tanto para jogar sem perda de FPS quanto para assistir filmes e animes sincronizados com a galera.

### 🎙️ Chamada de Voz de Baixa Latência & Áudio do Sistema
- **VoIP UDP Direto:** Voz tratada em C com baixíssimo buffer.
- **Detecção de Voz Ativa (Green Ring):** Borda verde animada na lista de participantes que acende em tempo real quando você fala.
- **Captura Loopback do Sistema:** Quer mostrar o som do jogo ou daquele vídeo do YouTube? Basta ativar o loopback que ele captura a saída de áudio das suas caixas/fone e joga na call com mixagem limpa.

### 🎯 Ferramentas de Estudo e Foco
Não é só para jogos — a gente usa muito para sessões de estudo em grupo e pair programming:
- **Pomodoro Integrado:** Timer clássico com avisos sonoros (chimes) para manter o ritmo de estudo e pausa.
- **Quadro de Tarefas (Kanban leve):** Marque pendências, adicione prints ou imagens de apoio e acompanhe quem fez o quê.
- **Mural de Notas & Imagens:** Compartilhe fotos, anotações de aula, trechos de código e GIFs.
- **Histórico Leve & Saudável:** Banco de dados SQLite local com limpeza inteligente a cada 90 dias (3 meses). Nada de entulhar gigabytes de dados inúteis no seu disco.

### 🔒 Criptografia Ponta a Ponta de Verdade (E2EE)
Aqui não tem papo furado de marketing:
- **Derivação de Chaves com Argon2id:** Quando você define a senha da sala, o app usa o algoritmo **Argon2id** (com alto custo de memória de 128 MB) para gerar uma chave mestra de 256 bits resistente a força bruta com GPUs.
- **Zero-Knowledge Handshake:** A senha da sala **nunca viaja pela rede**. Nem mesmo um hash simples dela é enviado! Usamos um desafio HMAC criptográfico onde ambos os lados provam que conhecem a chave sem expô-la.
- **Chaves Independentes via HKDF-SHA256:** Canais separados para TCP (mensagens/estado), Áudio (UDP) e Vídeo (UDP).
- **AES-256-GCM + Janela Anti-Replay:** Todo pacote de áudio e vídeo possui autenticação contra adulteração (AAD) e proteção contra repetição de pacotes maliciosos. Nem quem hospeda a sala consegue bisbilhotar o que passa se não tiver a senha correta.

---

## 🚀 Guia Rápido: Como Usar

O app foi pensado para funcionar em qualquer **Rede Local (LAN)** física ou em **VPNs Virtuais seguras** (nossa recomendação para jogar com amigos à distância é o [Radmin VPN](https://www.radmin-vpn.com/) ou [ZeroTier](https://www.zerotier.com/)).

### 1️⃣ Modo Host (Criar Sala)
1. Abra o **Radmin VPN** e crie uma rede para sua galera (ou use a sua rede Wi-Fi/cabo local).
2. Abra o aplicativo. Ele já identifica automaticamente o seu IP virtual do Radmin (começado com `26.x.x.x`).
3. Selecione a opção **"Hospedar sala"**.
4. Clique em **"Copiar meu IP"** e use o botão **"Gerar Forte"** para criar uma senha ultra-segura de 6 palavras fáceis de ditar.
5. Clique em **"Entrar"** e envie o IP e a senha para seus amigos.

### 2️⃣ Modo Cliente (Entrar na Sala)
1. Entre na mesma rede do Radmin VPN (ou mesma rede local) do Host.
2. Abra o app, digite seu apelido e selecione **"Entrar em uma sala"**.
3. Cole o IP do host no campo de endereço e insira a senha da sala.
4. Clique em **"Entrar"**. Em frações de segundo o handshake criptográfico é validado e vocês já estão na sala!

### 📺 Dicas para Transmissão de Tela
- Ao clicar no botão **"Compartilhar Tela"**, você pode escolher transmitir o **Monitor Inteiro** ou uma **Janela Específica** de jogo ou navegador.
- Escolha a resolução desejada (**720p a 60 FPS** para economia de banda em conexões moderadas ou **1080p a 60 FPS** para máxima fidelidade gráfica).

> [!TIP]
> Se o jogo estiver em tela cheia exclusiva, sugerimos colocar o jogo em modo *Janela Sem Bordas (Borderless Window)* para a captura DXGI rodar com a melhor taxa de quadros possível!

---

## 💻 Compatibilidade (Windows & Linux)

| Sistema Operacional | Status | Notas de Uso |
| :--- | :---: | :--- |
| **Windows 10 / 11 (64-bit)** | 🟢 Nativo Completo | Suporte total com aceleração DirectX DXGI, WASAPI e aceleração de hardware via Media Foundation. Executável portátil pronto para uso. |
| **Linux (Debian, Ubuntu, Kali, Arch)** | 🟡 Via LAN / ZeroTier | Funciona com interface Python e conexão de rede cliente/servidor. Para o streaming de vídeo/áudio no Linux, o pipeline de captura requer drivers V4L2/PulseAudio/PipeWire compatíveis. |

> [!NOTE]
> Usuários Linux podem rodar tranquilamente via terminal conectando-se a hosts Windows através de redes ZeroTier ou VPNs locais.

---

## 🛠️ Como Compilar do Código-Fonte

Se você curte botar a mão na massa, inspecionar o código ou compilar seus próprios binários, siga o passo a passo abaixo:

### Pré-requisitos
- **Python 3.10 ou superior** instalado.
- Um compilador C para Windows:
  - **MinGW-w64 (GCC)** adicionado ao seu `PATH` **OU**
  - **MSVC (Visual Studio Build Tools)**.

### 1. Clonar o repositório e instalar dependências Python
```powershell
# Clone o projeto
git clone https://github.com/seu-usuario/app-de-call.git
cd app-de-call

# Crie e ative um ambiente virtual (opcional, mas recomendado)
python -m venv venv
.\venv\Scripts\activate

# Instale os pacotes necessários
pip install -r requirements.txt
```

### 2. Compilar os Motores Nativos em C (`.dll`)

O projeto possui dois motores em C com scripts automáticos de build que detectam GCC ou MSVC e aplicam flags de segurança moderna (*Stack Canaries*, *ASLR/DynamicBase*, *DEP/NXCompat*):

#### Compilar o Motor de Áudio (`audio_engine`):
```powershell
cd audio_engine
.\build.bat
cd ..
```
*Gera `audio_engine.dll` com suporte a WASAPI e Windows Crypto CNG (`bcrypt`).*

#### Compilar o Motor de Vídeo (`video_engine`):
```powershell
cd video_engine
.\build.bat
cd ..
```
*Gera `video_engine.dll` com suporte a DXGI 1.2, Direct3D 11 e Media Foundation H.264.*

### 3. Rodar o aplicativo
```powershell
python main.py
```

Pronto! A interface inicial abrirá já solicitando a criação ou entrada na sala.

---

## 🗂️ Estrutura do Projeto

```text
├── audio_engine/        # Motor de VoIP de baixa latência em C (WASAPI + VAD + CNG Crypto)
│   ├── audio_engine.c
│   ├── build.bat
│   └── Makefile
├── video_engine/        # Motor de captura e streaming 60 FPS em C (DXGI + MF H.264)
│   ├── video_engine.c
│   ├── build.bat
│   └── Makefile
├── network/             # Protocolos de rede, E2EE e pontes C-Python
│   ├── crypto.py        # Derivação Argon2id, HKDF, AES-256-GCM e replay window
│   ├── protocol.py      # Definição dos pacotes e frames TCP/UDP
│   ├── audio_bridge.py  # ctypes bridge para audio_engine.dll
│   ├── video_bridge.py  # ctypes bridge para video_engine.dll
│   ├── host.py          # Servidor da sala e distribuição de streams
│   └── client.py        # Cliente P2P
├── database/            # Persistência local SQLite com retenção de 90 dias
│   ├── store.py
│   └── media.py
├── ui/                  # Interface gráfica moderna em PyQt6
│   ├── main_window.py   # Janela principal e indicador Green Ring
│   ├── panels.py        # Chat com suporte a imagens, Pomodoro e Quadro de Tarefas
│   ├── video_view.py    # Renderizador das telas compartilhadas
│   └── style.qss        # Tema escuro elegante
├── requirements.txt     # Dependências Python mínimas
└── main.py              # Ponto de entrada da aplicação
```

---

## ⚖️ Licença e Aviso Legal

Distribuído sob a licença **MIT**. Isso significa que o projeto é 100% de código aberto: você pode usar, estudar, modificar, forkar e distribuir livremente para jogar e estudar com seus amigos.

---

<p align="center">
  Feito com café, C e Python para quem valoriza desempenho de verdade e privacidade na rede. ☕⚡
</p>
