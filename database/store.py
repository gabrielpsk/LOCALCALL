"""Persistência local (SQLite). Cada instância tem seu próprio arquivo."""
import re
import sqlite3
import sys
from pathlib import Path

from .media import MAX_BYTES, make_name, sniff_ext, valid_name

if getattr(sys, "frozen", False):
    DATA_DIR = Path(sys.executable).resolve().parent / "data"
else:
    DATA_DIR = Path(__file__).resolve().parent.parent / "data"

SCHEMA = """
CREATE TABLE IF NOT EXISTS chat (
    id INTEGER PRIMARY KEY,
    author TEXT,
    text TEXT,
    ts REAL,
    image TEXT,
    reply_to_id INTEGER,
    reply_author TEXT,
    reply_text TEXT
);
CREATE TABLE IF NOT EXISTS tasks (
    id INTEGER PRIMARY KEY,
    text TEXT,
    done INTEGER,
    author TEXT,
    image TEXT
);
CREATE TABLE IF NOT EXISTS notes (
    id INTEGER PRIMARY KEY,
    title TEXT,
    body TEXT,
    author TEXT,
    ts REAL
);
"""
TABLES = ("chat", "tasks", "notes")


class Store:
    def __init__(self, user: str):
        DATA_DIR.mkdir(exist_ok=True)
        safe = re.sub(r"[^\w\-]", "_", user)
        self.db = sqlite3.connect(DATA_DIR / f"{safe}.db")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(SCHEMA)
        self._migrate()
        # Imagens ficam em arquivos; o banco guarda só o nome (hash)
        self.media_dir = DATA_DIR / f"{safe}_media"
        self.media_dir.mkdir(exist_ok=True)
        self.cleanup_old_records()

    def _migrate(self):
        """Adiciona colunas novas em bancos existentes sem perder dados."""
        migrations = {
            "chat": [
                ("image", "TEXT"),
                ("reply_to_id", "INTEGER"),
                ("reply_author", "TEXT"),
                ("reply_text", "TEXT"),
            ],
            "tasks": [
                ("image", "TEXT"),
            ],
        }
        for table, extras in migrations.items():
            cols = {r["name"] for r in self.db.execute(f"PRAGMA table_info({table})")}
            for col_name, col_type in extras:
                if col_name not in cols:
                    self.db.execute(f"ALTER TABLE {table} ADD COLUMN {col_name} {col_type}")
        self.db.commit()

    def cleanup_old_records(self):
        """Remove mensagens/notas com mais de 90 dias e apaga arquivos de mídia órfãos."""
        with self.db:
            self.db.execute("DELETE FROM chat WHERE ts < CAST(strftime('%s', 'now', '-90 days') AS REAL)")
            self.db.execute("DELETE FROM notes WHERE ts < CAST(strftime('%s', 'now', '-90 days') AS REAL)")

        used = {
            r[0]
            for r in self.db.execute(
                "SELECT image FROM chat WHERE image IS NOT NULL UNION "
                "SELECT image FROM tasks WHERE image IS NOT NULL"
            )
        }
        for f in self.media_dir.iterdir():
            if f.is_file() and valid_name(f.name) and f.name not in used:
                try:
                    f.unlink()
                except OSError:
                    pass

    def _rows(self, sql, args=()):
        return [dict(r) for r in self.db.execute(sql, args)]

    def chat(self):
        return self._rows("SELECT * FROM chat ORDER BY ts, id")

    def tasks(self):
        return self._rows("SELECT * FROM tasks ORDER BY done, id")

    def notes(self):
        return self._rows("SELECT * FROM notes ORDER BY title COLLATE NOCASE")

    def get(self, table, row_id):
        rows = self._rows(f"SELECT * FROM {table} WHERE id = ?", (row_id,))
        return rows[0] if rows else None

    def next_id(self, table):
        row = self.db.execute(f"SELECT COALESCE(MAX(id), 0) + 1 FROM {table}").fetchone()
        return row[0]

    def snapshot(self):
        return {"chat": self.chat(), "tasks": self.tasks(), "notes": self.notes()}

    # --- imagens (só tocam o disco, seguras para uso em threads) ---
    def media_path(self, name) -> Path:
        return self.media_dir / name

    def has_media(self, name) -> bool:
        return bool(name) and self.media_path(name).exists()

    def save_media(self, data: bytes):
        """Valida tipo/tamanho e grava o arquivo. Retorna o nome, ou None se inválido."""
        ext = sniff_ext(data)
        if not ext or len(data) > MAX_BYTES:
            return None
        name = make_name(data, ext)
        path = self.media_path(name)
        if not path.exists():
            tmp = path.with_suffix(".part")
            tmp.write_bytes(data)
            tmp.replace(path)
        return name

    def missing_media(self):
        """Imagens citadas no banco que ainda não existem no disco."""
        rows = self.db.execute("SELECT image FROM chat WHERE image IS NOT NULL UNION "
                               "SELECT image FROM tasks WHERE image IS NOT NULL")
        return [r[0] for r in rows if not self.has_media(r[0])]

    def _merge_chat_row(self, r: dict):
        """Faz merge inteligente no chat sem apagar histórico local de 90 dias."""
        reply = r.get("reply_to") if isinstance(r.get("reply_to"), dict) else None
        reply_to_id = r.get("reply_to_id") if r.get("reply_to_id") is not None else (reply.get("id") if reply else None)
        reply_author = r.get("reply_author") or (reply.get("author") if reply else None)
        reply_text = r.get("reply_text") if r.get("reply_text") is not None else (reply.get("text") if reply else None)

        # Se já existe a mesma mensagem por (author, ts, text), ignora duplicata
        dup = self.db.execute(
            "SELECT id FROM chat WHERE author = ? AND ABS(ts - ?) < 1e-4 AND COALESCE(text, '') = COALESCE(?, '')",
            (r["author"], r["ts"], r.get("text", "")),
        ).fetchone()
        if dup:
            return

        # Se o ID já estiver ocupado por uma mensagem antiga local diferente, aloca novo ID local
        row_id = r.get("id")
        existing = self.get("chat", row_id) if row_id is not None else None
        if existing:
            row_id = self.next_id("chat")

        self.db.execute(
            "INSERT OR IGNORE INTO chat (id, author, text, ts, image, reply_to_id, reply_author, reply_text) "
            "VALUES (?,?,?,?,?,?,?,?)",
            (row_id, r["author"], r.get("text", ""), r["ts"], r.get("image"),
             reply_to_id, reply_author, reply_text),
        )

    def apply(self, ev):
        """Aplica um evento confirmado pelo host (igual no host e nos clientes)."""
        t = ev["t"]
        with self.db:
            if t == "chat":
                reply = ev.get("reply_to") if isinstance(ev.get("reply_to"), dict) else None
                reply_to_id = ev.get("reply_to_id") if ev.get("reply_to_id") is not None else (reply.get("id") if reply else None)
                reply_author = ev.get("reply_author") or (reply.get("author") if reply else None)
                reply_text = ev.get("reply_text") if ev.get("reply_text") is not None else (reply.get("text") if reply else None)
                self.db.execute(
                    "INSERT OR REPLACE INTO chat (id, author, text, ts, image, reply_to_id, reply_author, reply_text) "
                    "VALUES (?,?,?,?,?,?,?,?)",
                    (ev["id"], ev["author"], ev["text"], ev["ts"], ev.get("image"),
                     reply_to_id, reply_author, reply_text),
                )
            elif t == "chat_del":
                msg_id = ev.get("id")
                orig = self.get("chat", msg_id)
                if orig and orig.get("image"):
                    img_name = orig["image"]
                    # Só remove arquivo de imagem se não estiver em uso em outra mensagem/tarefa
                    self.db.execute("DELETE FROM chat WHERE id = ?", (msg_id,))
                    other_use = self.db.execute(
                        "SELECT 1 FROM chat WHERE image = ? UNION SELECT 1 FROM tasks WHERE image = ?",
                        (img_name, img_name)
                    ).fetchone()
                    if not other_use:
                        p = self.media_path(img_name)
                        if p.exists():
                            try:
                                p.unlink()
                            except OSError:
                                pass
                else:
                    self.db.execute("DELETE FROM chat WHERE id = ?", (msg_id,))
            elif t == "task":
                self.db.execute("INSERT OR REPLACE INTO tasks (id, text, done, author, image) "
                                "VALUES (?,?,?,?,?)",
                                (ev["id"], ev["text"], int(ev["done"]), ev["author"],
                                 ev.get("image")))
            elif t == "task_del":
                self.db.execute("DELETE FROM tasks WHERE id = ?", (ev["id"],))
            elif t == "note":
                self.db.execute("INSERT OR REPLACE INTO notes VALUES (?,?,?,?,?)",
                                (ev["id"], ev["title"], ev["body"], ev["author"], ev["ts"]))
            elif t == "note_del":
                self.db.execute("DELETE FROM notes WHERE id = ?", (ev["id"],))
            elif t == "snapshot":
                # Merge inteligente em chat e notes (mantém histórico local de 90 dias); sincroniza tasks ativas
                self.db.execute("DELETE FROM tasks")
                for r in ev.get("chat", []):
                    self._merge_chat_row(r)
                for r in ev.get("tasks", []):
                    self.apply_row("tasks", r)
                for r in ev.get("notes", []):
                    self.apply_row("notes", r)

    def apply_row(self, table, row):
        cols = ",".join(row)
        marks = ",".join("?" * len(row))
        self.db.execute(f"INSERT OR REPLACE INTO {table} ({cols}) VALUES ({marks})", tuple(row.values()))
