"""Nonblocking per-config interprocess lock; OS releases it after crashes."""
from contextlib import contextmanager
import os


class OperationBusy(RuntimeError):
    pass


@contextmanager
def operation_lock(path):
    name = os.path.abspath(path) + '.lock'
    os.makedirs(os.path.dirname(name),exist_ok=True)
    stream = os.fdopen(os.open(name,os.O_CREAT|os.O_RDWR,0o600),'r+b')
    try:
        if os.fstat(stream.fileno()).st_size == 0:
            stream.write(b'0');stream.flush()
        stream.seek(0)
        try:
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(stream.fileno(),msvcrt.LK_NBLCK,1)
            else:
                import fcntl
                fcntl.flock(stream.fileno(),fcntl.LOCK_EX|fcntl.LOCK_NB)
        except OSError:
            raise OperationBusy('另一个实例正在操作此配置，请等待完成后再试') from None
        try:
            yield
        finally:
            stream.seek(0)
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(stream.fileno(),msvcrt.LK_UNLCK,1)
            else:
                import fcntl
                fcntl.flock(stream.fileno(),fcntl.LOCK_UN)
    finally:
        stream.close()
