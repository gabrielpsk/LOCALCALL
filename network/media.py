"""Executa trabalho pesado (disco, base64) fora da thread da interface."""
from PyQt6.QtCore import QObject, QRunnable, QThreadPool, pyqtSignal


class _Bridge(QObject):
    """Vive na thread principal: entrega o resultado do worker para o callback."""
    finished = pyqtSignal(object, object)

    def __init__(self):
        super().__init__()
        self.finished.connect(self._deliver)

    def _deliver(self, cb, result):
        if callable(cb):
            try:
                cb(result)
            except Exception:
                pass


_bridge = None


class _Job(QRunnable):
    def __init__(self, fn, callback=None):
        super().__init__()
        self.fn, self.callback = fn, callback

    def run(self):
        try:
            result = self.fn()
        except Exception:
            result = None
        if self.callback is not None and _bridge is not None:
            _bridge.finished.emit(self.callback, result)


def run_async(fn, callback=None):
    """Roda fn() numa thread; callback(resultado) é chamado na thread principal.
    O resultado é None se fn falhar."""
    global _bridge
    if _bridge is None:
        _bridge = _Bridge()          # criado na thread principal
    QThreadPool.globalInstance().start(_Job(fn, callback))
