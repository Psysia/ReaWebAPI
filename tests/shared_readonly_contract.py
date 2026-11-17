"""Exercise the production native reader against other real writer processes.

All paths belong to TemporaryDirectory. The hold hook is test-build-only and
pauses the production function after snapshot/open, proving Windows sharing
while its actual OS handle is alive rather than only between calls.
"""
from contextlib import closing
import hashlib
import json
import pathlib
import sqlite3
import subprocess
import sys
import tempfile


def sqlite_writer(path, count):
    with closing(sqlite3.connect(path, timeout=0.0)) as database:
        for index in range(count):
            database.execute("UPDATE items SET value=? WHERE id=?", (f"external-{index}", index % 100 + 1))
            database.commit()
    print(count, flush=True)


class Reader:
    def __init__(self, executable):
        self.process = subprocess.Popen([executable], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True, encoding="utf-8")

    def send(self, line):
        self.process.stdin.write(line + "\n")
        self.process.stdin.flush()

    def request(self, path, offset=0, count=65536, mode="read"):
        self.send(f"{mode}\t{path}\t{offset}\t{count}")
        if mode == "hold":
            assert self.process.stdout.readline().strip() == "HANDLE_OPEN"
            return None
        return self.result()

    def result(self):
        return json.loads(self.process.stdout.readline())

    def release(self):
        self.send("release")
        return self.result()

    def close(self):
        self.process.stdin.close()
        assert self.process.wait(timeout=15) == 0, self.process.stderr.read()


def main(executable):
    reader = Reader(executable)
    try:
        with tempfile.TemporaryDirectory(prefix="reaweb-shared-read-") as temporary:
            root = pathlib.Path(temporary)
            source = root / "source-音效-ä.bin"
            original = bytes(range(256)) * 4096
            source.write_bytes(original)
            before = hashlib.sha256(source.read_bytes()).hexdigest()
            names = sorted(path.name for path in root.iterdir())
            first = reader.request(source, 17, 65536)
            assert first["ok"] and bytes.fromhex(first["dataHex"]) == original[17:17 + 65536]
            assert first["size"] == str(len(original)) and first["fileId"]
            assert reader.request(source, 0, 0)["dataHex"] == ""
            assert reader.request(source, len(original), 1)["dataHex"] == ""
            assert bytes.fromhex(reader.request(source, len(original) - 3, 65536)["dataHex"]) == original[-3:]
            for offset, count in [(-1, 1), ("no", 1), ("9223372036854775808", 1), (0, -1), (0, 65537)]:
                assert reader.request(source, offset, count)["code"] == "INVALID_ARGUMENT"
            assert reader.request(root / "missing", 0, 0)["code"] == "NOT_FOUND"
            assert reader.request("relative.bin", 0, 0)["code"] == "INVALID_ARGUMENT"
            assert not reader.request(root, 0, 0)["ok"]
            assert reader.request(source, mode="threads")["ok"]
            assert hashlib.sha256(source.read_bytes()).hexdigest() == before
            assert sorted(path.name for path in root.iterdir()) == names, "reader created a sidecar"

            # A write from a different process completes while the actual reader
            # handle is paused open; the changed snapshot must be rejected.
            reader.request(source, mode="hold")
            subprocess.run([sys.executable, "-c", "import pathlib,sys; pathlib.Path(sys.argv[1]).write_bytes(b'changed')", str(source)], check=True)
            assert reader.release()["code"] == "FILE_CHANGED"
            source.write_bytes(original)
            before_identity = reader.request(source, 0, 0)["fileId"]
            reader.request(source, mode="hold")
            renamed = root / "renamed.bin"
            subprocess.run([sys.executable, "-c", "import pathlib,sys; p=pathlib.Path(sys.argv[1]); p.rename(sys.argv[2]); p.write_bytes(b'replacement')", str(source), str(renamed)], check=True)
            held = reader.release()
            assert held["ok"] or held["code"] == "FILE_CHANGED"
            assert reader.request(source, 0, 0)["fileId"] != before_identity
            reader.request(source, mode="hold")
            subprocess.run([sys.executable, "-c", "import pathlib,sys; pathlib.Path(sys.argv[1]).unlink()", str(source)], check=True)
            held = reader.release()
            assert held["ok"] or held["code"] == "FILE_CHANGED"
            assert reader.request(source, 0, 0)["code"] == "NOT_FOUND"

            database = root / "concurrent.sqlite"
            with closing(sqlite3.connect(database)) as connection:
                connection.execute("CREATE TABLE items(id INTEGER PRIMARY KEY,value TEXT)")
                connection.executemany("INSERT INTO items VALUES(?,?)", ((i, "original" * 20) for i in range(1, 101)))
                connection.commit()
            reader.request(database, mode="hold")
            completed = subprocess.run([sys.executable, __file__, "--writer", str(database), "25"],
                                       capture_output=True, text=True, check=True, timeout=15)
            assert completed.stdout.strip() == "25"
            assert reader.release()["code"] == "FILE_CHANGED"
            writer = subprocess.Popen([sys.executable, __file__, "--writer", str(database), "300"],
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            concurrent_reads = 0
            while writer.poll() is None:
                response = reader.request(database)
                assert response["ok"] or response["code"] in {"FILE_CHANGED", "READ_FAILED"}
                concurrent_reads += 1
            out, err = writer.communicate(timeout=15)
            assert writer.returncode == 0 and out.strip() == "300", err
            assert concurrent_reads > 0
            with closing(sqlite3.connect(database)) as connection:
                assert connection.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
            # Holding a WAL read must not block SQLite's normal close/checkpoint
            # cleanup. Only the external writer executes journal-mode SQL.
            with closing(sqlite3.connect(database)) as connection:
                assert connection.execute("PRAGMA journal_mode=WAL").fetchone()[0] == "wal"
                connection.execute("UPDATE items SET value='wal' WHERE id=1"); connection.commit()
                wal = pathlib.Path(str(database) + "-wal")
                reader.request(wal, mode="hold")
            held = reader.release()
            assert held["ok"] or held["code"] == "FILE_CHANGED"
            assert not wal.exists(), "reader blocked SQLite WAL cleanup"
            print(f"Shared native source reads: PASS (Unicode/bounds/8 threads/325 SQLite commits/{concurrent_reads} concurrent reads/held-handle overwrite, rename, delete/WAL cleanup)")
    finally:
        reader.close()


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--writer":
        sqlite_writer(sys.argv[2], int(sys.argv[3]))
    else:
        main(sys.argv[1])
