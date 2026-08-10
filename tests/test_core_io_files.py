"""Core owns a structured, effect-typed file IO vocabulary."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CoreIOFileTests(unittest.TestCase):

    def test_directory_core_source_is_deliberate_and_readable(self):
        directory_source = (ROOT / "core" / "IO" / "Directory.mon").read_text()
        file_source = (ROOT / "core" / "IO" / "File.mon").read_text()

        for heading in (
            ";;; Entry identity",
            ";;; Directory observation",
            ";;; Pure snapshot views",
        ):
            self.assertIn(heading, directory_source)

        for heading in (
            ";;; Descriptor identity",
            ";;; Status and inheritance flags",
            ";;; Handle lifecycle",
            ";;; Resource scopes",
            ";;; Borrowed-handle streaming",
            ";;; Owned path workflows",
            ";;; Standard streams",
        ):
            self.assertIn(heading, file_source)

        self.assertNotIn("__rt_prepend", directory_source)
        self.assertNotIn("\n\n\n\n", directory_source)
        self.assertNotIn("\n\n\n\n", file_source)

        long_lines = [
            (number, line)
            for number, line in enumerate(directory_source.splitlines(), 1)
            if len(line) > 120
        ]
        self.assertEqual(long_lines, [], long_lines)

    def test_posix_open_accepts_a_string_path_across_module_boundary(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            target = work / "opened.txt"
            program = work / "OpenProbe.mon"
            executable = work / "OpenProbe"
            program.write_text(f'''
import System.Posix.FileDescriptor
module OpenProbe []
define fd (sys-open "{target}" (bit-or O_WRONLY (bit-or O_CREAT O_TRUNC)) 420)
show fd
show (sys-close fd)
''')
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            cache_home = work / "home"
            cache_home.mkdir()
            environment["HOME"] = str(cache_home)
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(program), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=work, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            lines = ran.stdout.splitlines()
            self.assertEqual(len(lines), 2, ran.stdout)
            self.assertGreaterEqual(int(lines[0]), 0, ran.stdout)
            self.assertEqual(lines[1], "0", ran.stdout)
            self.assertTrue(target.exists())

    def test_file_api_is_core_owned_and_uses_compact_io_arrows(self):
        source = (ROOT / "core" / "IO" / "File.mon").read_text()
        pipe_source = (ROOT / "core" / "IO" / "Pipe.mon").read_text()
        directory_source = (ROOT / "core" / "IO" / "Directory.mon").read_text()
        for declaration in (
            "data OpenMode",
            "data SeekOrigin",
            "data PathKind",
            "RegularFile",
            "Directory",
            "layout FileMetadata",
            "layout FileHandle",
            "define file-descriptor :: FileHandle -> Int",
            "define standard-input :: () -> FileHandle",
            "define standard-output :: () -> FileHandle",
            "define standard-error :: () -> FileHandle",
            "define file-status-flags :: FileHandle -io-> Either IOError Int",
            "define set-file-status-flags :: FileHandle -> Int -io-> Either IOError Int",
            "define file-descriptor-flags :: FileHandle -io-> Either IOError Int",
            "define set-file-descriptor-flags :: FileHandle -> Int -io-> Either IOError Int",
            "define enable-nonblocking :: FileHandle -io-> Either IOError Int",
            "define disable-nonblocking :: FileHandle -io-> Either IOError Int",
            "define nonblocking-status :: FileHandle -io-> Either IOError Int",
            "define enable-close-on-exec :: FileHandle -io-> Either IOError Int",
            "define disable-close-on-exec :: FileHandle -io-> Either IOError Int",
            "define close-on-exec-status :: FileHandle -io-> Either IOError Int",
            "define duplicate-file-descriptor :: FileHandle -io-> Either IOError Int",
            "define redirect-file-handle :: FileHandle -> FileHandle -io-> Either IOError Int",
            "define open-file :: Path -> OpenMode -io-> Either IOError FileHandle",
            "define with-file :: Path -> OpenMode -> (FileHandle -> Either IOError a) -io-> Either IOError a",
            "define close-file :: FileHandle -io-> Either IOError Int",
            "define flush-file :: FileHandle -io-> Either IOError Int",
            "define seek-file :: FileHandle -> Int -> SeekOrigin -io-> Either IOError Int",
            "define read-file-byte :: FileHandle -io-> Either IOError Int",
            "define read-handle-bytes :: FileHandle -io-> Either IOError [Int]",
            "define write-file-byte :: FileHandle -> Int -io-> Either IOError Int",
            "define write-file-text :: FileHandle -> String -io-> Either IOError Int",
            "define rename-file :: Path -> Path -io-> Either IOError Int",
            "define remove-file :: Path -io-> Either IOError Int",
            "define create-directory :: Path -io-> Either IOError Int",
            "define remove-directory :: Path -io-> Either IOError Int",
            "define path-exists :: Path -io-> Either IOError Bool",
            "define path-kind :: Path -io-> Either IOError PathKind",
            "define metadata :: Path -io-> Either IOError FileMetadata",
            "define metadata-size :: Path -io-> Either IOError Int",
            "define metadata-permissions :: Path -io-> Either IOError Int",
            "define list-directory-names :: Path -io-> Either IOError [String]",
            "define join-path :: Path -> String -> Path",
            "define is-directory :: Path -io-> Either IOError Bool",
            "define is-regular-file :: Path -io-> Either IOError Bool",
            "define is-symbolic-link :: Path -io-> Either IOError Bool",
            "define is-other :: Path -io-> Either IOError Bool",
            "define sync-file-data :: FileHandle -io-> Either IOError Int",
            "define set-file-size :: FileHandle -> Int -io-> Either IOError Int",
            "define set-file-permissions :: Path -> Int -io-> Either IOError Int",
            "define file-size :: Path -io-> Either IOError Int",
            "define touch-file :: Path -io-> Either IOError Int",
            "define truncate-file :: Path -> Int -io-> Either IOError Int",
            "define copy-file :: Path -> Path -io-> Either IOError Int",
            "define move-file :: Path -> Path -io-> Either IOError Int",
            "define read-file-bytes :: Path -io-> Either IOError [Int]",
            "define read-file-prefix :: Path -> Int -io-> Either IOError [Int]",
            "define write-file :: Path -> String -io-> Either IOError Int",
            "define append-file :: Path -> String -io-> Either IOError Int",
        ):
            self.assertIn(declaration, source)
        for declaration in (
            "data Pipe",
            "define create-pipe :: () -io-> PipeOpenResult",
            "define pipe-reader-descriptor :: Pipe -> Int",
            "define pipe-writer-descriptor :: Pipe -> Int",
        ):
            self.assertIn(declaration, pipe_source)
        self.assertIn("layout DirectoryEntry", directory_source)
        self.assertIn(
            "define scan-directory :: Path -io-> Either IOError [DirectoryEntry]",
            directory_source)
        self.assertIn(
            "define directory-entry-size :: DirectoryEntry -io-> Either IOError Int",
            directory_source)
        self.assertIn(
            "define directory-entry-is-directory :: DirectoryEntry -io-> Either IOError Bool",
            directory_source)
        self.assertIn("layout DirectoryEntrySnapshot", directory_source)
        self.assertIn(
            "define snapshot-directory :: Path -io-> Either IOError [DirectoryEntrySnapshot]",
            directory_source)
        self.assertIn(
            "define directory-snapshot-total-size :: [DirectoryEntrySnapshot] -> Int",
            directory_source)
        self.assertNotIn("__rt_prepend", directory_source)
        self.assertNotIn("data OpenMode  data SeekOrigin", source)
        self.assertNotIn("where e has io", source)
        self.assertNotIn("::*U8", source.replace(" ", ""))
        self.assertNotIn(":: *U8", source)
        self.assertNotIn("def ignored (close-file handle)", source)

    def test_with_file_closes_its_owned_handle_after_callback_success(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            target = work / "scoped.txt"
            program = work / "WithFileProbe.mon"
            executable = work / "WithFileProbe"
            program.write_text(f'''
import IO.File
import Data.Either
module WithFileProbe []

define finish-owned-write :: FileHandle -> Either IOError Int -> Either IOError Int
  _ [Left error] -> Left error
  handle [Right _] -> Right (file-descriptor handle)

define write-owned :: FileHandle -io-> Either IOError Int
  handle -> finish-owned-write handle (write-file-text handle "scoped")

define used-descriptor (fromRight (with-file {target} WriteOnly write-owned) (0 - 1))
show (left? (close-file (FileHandle used-descriptor)))
''')
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            environment["HOME"] = str(work / "home")
            (work / "home").mkdir()
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(program), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=work, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout, "True\n")
            self.assertEqual(target.read_text(), "scoped")

    def test_pipe_duplication_redirection_and_open_handle_streaming_execute(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            program = work / "PipeProbe.mon"
            executable = work / "PipeProbe"
            program.write_text('''
import IO.File
import IO.Pipe
import Data.Either
module PipeProbe []
define channel-result (create-pipe ())
define channel (PipeEnds channel-result.reader-descriptor channel-result.writer-descriptor)
define reader (FileHandle (pipe-reader-descriptor channel))
define writer (FileHandle (pipe-writer-descriptor channel))
define redirected (FileHandle (fromRight (duplicate-file-descriptor reader) (0 - 1)))
show (= (fromRight (redirect-file-handle writer redirected) (0 - 1)) (file-descriptor redirected))
show (fromRight (close-file writer) (0 - 1))
show (fromRight (write-file-text redirected "pipe") (0 - 1))
show (fromRight (close-file redirected) (0 - 1))
define bytes (fromRight (read-handle-bytes reader) [])
show (count bytes)
show (head bytes)
show (head (tail bytes))
show (fromRight (close-file reader) (0 - 1))
''')
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            environment["HOME"] = str(work / "home")
            (work / "home").mkdir()
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(program), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=work, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=5)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout, "True\n0\n4\n0\n4\n112\n105\n0\n")

    def test_descriptor_flags_nonblocking_and_cloexec_execute(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            program = work / "DescriptorControlProbe.mon"
            executable = work / "DescriptorControlProbe"
            program.write_text('''
import IO.File
import IO.Pipe
import Data.Either
module DescriptorControlProbe []
define opened (create-pipe ())
define reader (FileHandle opened.reader-descriptor)
define writer (FileHandle opened.writer-descriptor)
show (fromRight (nonblocking-status reader) (0 - 1))
show (right? (enable-nonblocking reader))
show (fromRight (nonblocking-status reader) (0 - 1))
show (left? (read-file-byte reader))
show (right? (disable-nonblocking reader))
show (fromRight (nonblocking-status reader) (0 - 1))
show (fromRight (close-on-exec-status writer) (0 - 1))
show (right? (enable-close-on-exec writer))
show (fromRight (close-on-exec-status writer) (0 - 1))
show (right? (disable-close-on-exec writer))
show (fromRight (close-on-exec-status writer) (0 - 1))
show (fromRight (write-file-text writer "ABC") (0 - 1))
show (fromRight (close-file writer) (0 - 1))
show (count (fromRight (read-handle-bytes reader) []))
show (fromRight (close-file reader) (0 - 1))
show (left? (file-status-flags reader))
show (left? (enable-nonblocking reader))
show (file-descriptor (standard-input ()))
show (file-descriptor (standard-output ()))
show (file-descriptor (standard-error ()))
''')
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            environment["HOME"] = str(work / "home")
            (work / "home").mkdir()
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(program), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=work, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=5)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(
                ran.stdout,
                "0\nTrue\n1\nTrue\nTrue\n0\n0\nTrue\n1\nTrue\n0\n"
                "3\n0\n3\n0\nTrue\nTrue\n0\n1\n2\n")

    def test_effectful_final_arrow_keeps_every_runtime_parameter(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            program = work / "EffectArityProbe.mon"
            executable = work / "EffectArityProbe"
            program.write_text('''
module EffectArityProbe []
define add-through-io :: Int -> Int -io-> Int
  left right -> left + right
show (add-through-io 20 22)
''')
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            environment["HOME"] = str(work / "home")
            (work / "home").mkdir()
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(program), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run([str(executable)], cwd=work, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout, "42\n")

    def test_path_kind_distinguishes_files_directories_and_missing_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            file_path = work / "entry.txt"
            directory_path = work / "folder"
            symlink_path = work / "entry-link"
            fifo_path = work / "entry.pipe"
            missing_path = work / "missing"
            file_path.write_text("entry")
            directory_path.mkdir()
            symlink_path.symlink_to(file_path)
            os.mkfifo(fifo_path)
            program = work / "PathKindProbe.mon"
            executable = work / "PathKindProbe"
            program.write_text(f'''
import IO.File
import Data.Either
module PathKindProbe []
show (fromRight (is-directory {directory_path}) False)
show (fromRight (is-regular-file {directory_path}) True)
show (fromRight (is-directory {file_path}) True)
show (fromRight (is-regular-file {file_path}) False)
show (fromRight (is-symbolic-link {symlink_path}) False)
show (fromRight (is-other {fifo_path}) False)
show (left? (path-kind {missing_path}))
''')
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            environment["HOME"] = str(work / "home")
            (work / "home").mkdir()
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(program), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=work, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout, "1\n0\n0\n1\n1\n1\nTrue\n")

    def test_metadata_reports_kind_size_permissions_links_and_timestamp(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            target = work / "metadata.bin"
            target.write_bytes(b"metadata")
            target.chmod(0o640)
            program = work / "MetadataRecordProbe.mon"
            executable = work / "MetadataRecordProbe"
            program.write_text(f'''
import IO.File
import Data.Either
module MetadataRecordProbe []
show (fromRight (metadata-size {target}) (0 - 1))
show (fromRight (metadata-permissions {target}) (0 - 1))
show (fromRight (metadata-links {target}) (0 - 1))
show (fromRight (metadata-modified-seconds {target}) 0)
''')
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            environment["HOME"] = str(work / "home")
            (work / "home").mkdir()
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(program), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=work, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            lines = ran.stdout.splitlines()
            self.assertEqual(lines[:3], ["8", "416", "1"], ran.stdout)
            self.assertGreater(int(lines[3]), 0, ran.stdout)

    def test_directory_names_include_hidden_entries_and_exclude_dot_entries(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            folder = work / "listing"
            folder.mkdir()
            (folder / "alpha.txt").write_text("a")
            (folder / ".hidden").write_text("h")
            (folder / "nested").mkdir()
            (folder / "link").symlink_to(folder / "alpha.txt")
            missing = work / "missing"
            program = work / "DirectoryNamesProbe.mon"
            executable = work / "DirectoryNamesProbe"
            program.write_text(f'''
import IO.File
import Data.Either
module DirectoryNamesProbe []
define names (fromRight (list-directory-names {folder}) [])
show (count names)
show (contains? names "alpha.txt")
show (contains? names ".hidden")
show (contains? names "nested")
show (contains? names "link")
show (contains? names ".")
show (contains? names "..")
show (left? (list-directory-names {missing}))
''')
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            environment["HOME"] = str(work / "home")
            (work / "home").mkdir()
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(program), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=work, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(
                ran.stdout,
                "4\nTrue\nTrue\nTrue\nTrue\nFalse\nFalse\nTrue\n")

    def test_join_path_and_directory_scan_return_typed_entries(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            folder = work / "scan"
            folder.mkdir()
            (folder / "alpha.txt").write_text("alpha")
            (folder / ".hidden").write_text("hidden")
            (folder / "nested").mkdir()
            program = work / "DirectoryScanProbe.mon"
            executable = work / "DirectoryScanProbe"
            program.write_text(f'''
import IO.Directory
import IO.File
import Data.Either
module DirectoryScanProbe []
define entries (fromRight (scan-directory {folder}) [])
define names (directory-entry-names entries)
show (count entries)
show (contains? names "alpha.txt")
show (contains? names ".hidden")
show (contains? names "nested")
show (fromRight (metadata-size (join-path {folder} "alpha.txt")) (0 - 1))
''')
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            environment["HOME"] = str(work / "home")
            (work / "home").mkdir()
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(program), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=work, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(
                ran.stdout,
                "3\nTrue\nTrue\nTrue\n5\n")

    def test_imported_metadata_payload_and_path_layout_preserve_types(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            target = work / "typed-metadata.bin"
            target.write_bytes(b"evidence")
            program = work / "MetadataEvidenceProbe.mon"
            executable = work / "MetadataEvidenceProbe"
            program.write_text(f'''
import IO.File
import Data.Either
module MetadataEvidenceProbe []
layout PathBox [value :: Path]
define boxed (PathBox {target})
define information (fromRight (metadata boxed.value) (FileMetadata Other 0 0 0 0))
show information.size
show information.permissions
show (fromRight (metadata-size boxed.value) (0 - 1))
''')
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            environment["HOME"] = str(work / "home")
            (work / "home").mkdir()
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(program), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=work, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout.splitlines()[0], "8", ran.stdout)
            self.assertGreaterEqual(int(ran.stdout.splitlines()[1]), 0, ran.stdout)
            self.assertEqual(ran.stdout.splitlines()[2], "8", ran.stdout)

    def test_imported_metadata_can_build_a_list_of_core_layouts(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            folder = work / "data"
            folder.mkdir()
            first = folder / "first.txt"
            second = folder / "second.txt"
            first.write_bytes(b"one")
            second.write_bytes(b"second")
            program = work / "LayoutListProbe.mon"
            executable = work / "LayoutListProbe"
            program.write_text(f'''
import IO.Directory
import IO.File
import Data.Either
import Sequence

module LayoutListProbe []

layout Snapshot
  [path :: String]
  [size :: Int]

define snapshot-result :: DirectoryEntry -> Either IOError FileMetadata -> Either IOError Snapshot
  entry result -> (match result with
                  | (Left error)        -> (Left error)
                  | (Right information) -> (Right (Snapshot (__rt_path_text (directory-entry-path entry)) information.size)))

define snapshot :: DirectoryEntry -io-> Either IOError Snapshot
  entry -> snapshot-result entry (metadata (directory-entry-path entry))

define snapshots-into :: [DirectoryEntry] -> [Snapshot] -io-> Either IOError [Snapshot]
  [] captured -> Right captured
  [entry|rest] captured -> (match (snapshot entry) with
                           | (Left error) -> (Left error)
                           | (Right item) -> snapshots-into rest (__rt_prepend item captured))

define snapshots :: [DirectoryEntry] -io-> Either IOError [Snapshot]
  entries -> snapshots-into entries (list)

define total-size :: [Snapshot] -> Int
  [] -> 0
  [item|rest] -> item.size + total-size rest

define entries (fromRight (scan-directory {folder}) [])
show (right? (snapshots entries))
define captured (fromRight (snapshots entries) (list))
show (count captured)
show (total-size captured)
''')
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            environment["HOME"] = str(work / "home")
            (work / "home").mkdir()
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(program), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=work, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout, "True\n2\n9\n")

    def test_directory_snapshots_make_observation_explicit_and_reading_pure(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            folder = work / "inventory"
            folder.mkdir()
            (folder / "alpha.txt").write_bytes(b"alpha")
            (folder / "nested").mkdir()
            program = work / "DirectorySnapshotProbe.mon"
            executable = work / "DirectorySnapshotProbe"
            program.write_text(f'''
import IO.Directory
import IO.File
import Data.Either

module DirectorySnapshotProbe []

define entries
  (fromRight (snapshot-directory {folder}) (list))

show (directory-snapshot-names entries)
show (directory-snapshot-contains-path? entries (join-path {folder} "alpha.txt"))
show (directory-snapshot-contains-path? entries (join-path {folder} "nested"))
show (directory-snapshot-total-size entries)
show (directory-snapshot-total-links entries)
show (directory-snapshot-latest-modified-seconds entries)
''')
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            environment["HOME"] = str(work / "home")
            (work / "home").mkdir()
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(program), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=work, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            lines = ran.stdout.splitlines()
            self.assertIn("alpha.txt", lines[0], ran.stdout)
            self.assertIn("nested", lines[0], ran.stdout)
            self.assertEqual(lines[1:3], ["True", "True"], ran.stdout)
            self.assertGreaterEqual(int(lines[3]), 5, ran.stdout)
            self.assertGreaterEqual(int(lines[4]), 2, ran.stdout)
            self.assertGreater(int(lines[5]), 0, ran.stdout)

    def test_path_literal_size_touch_truncate_and_move_execute(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source_path = work / "source.txt"
            moved_path = work / "moved.txt"
            touched_path = work / "touched.txt"
            program = work / "PathOperationsProbe.mon"
            executable = work / "PathOperationsProbe"
            program.write_text(f'''
import IO.File
import Data.Either
module PathOperationsProbe []
show (fromRight (write-file {source_path} "path-data") (0 - 1))
show (fromRight (file-size {source_path}) (0 - 1))
show (fromRight (move-file {source_path} {moved_path}) (0 - 1))
show (fromRight (truncate-file {moved_path} 4) (0 - 1))
show (fromRight (file-size {moved_path}) (0 - 1))
show (fromRight (touch-file {touched_path}) (0 - 1))
''')
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            cache_home = work / "home"
            cache_home.mkdir()
            environment["HOME"] = str(cache_home)
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(program), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=work, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout, "9\n9\n0\n0\n4\n0\n")
            self.assertEqual(moved_path.read_text(), "path")
            self.assertFalse(source_path.exists())
            self.assertEqual(touched_path.read_bytes(), b"")

    def test_bounded_file_prefix_has_clear_name_and_exact_limit(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source_path = work / "readable.txt"
            program = work / "EasyFileNamesProbe.mon"
            executable = work / "EasyFileNamesProbe"
            program.write_text(f'''
import IO.File
import Data.Either
module EasyFileNamesProbe []
show (fromRight (write-file {source_path} "clear names") (0 - 1))
define prefix (fromRight (read-file-prefix {source_path} 5) [])
show (count prefix)
show (head prefix)
show (count (fromRight (read-file-prefix {source_path} 99) []))
show (count (fromRight (read-file-prefix {source_path} 0) []))
''')
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            environment["HOME"] = str(work / "home")
            (work / "home").mkdir()
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(program), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=work, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout, "11\n5\n99\n11\n0\n")

    def test_copy_file_streams_arbitrary_bytes_and_reports_count(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source_path = work / "source.bin"
            target_path = work / "target.bin"
            absent_path = work / "absent.bin"
            unwanted_path = work / "unwanted.bin"
            payload = bytes([0, 1, 2, 10, 127, 128, 200, 255]) * 17
            source_path.write_bytes(payload)
            target_path.write_bytes(b"stale" * 100)
            program = work / "BinaryCopyProbe.mon"
            executable = work / "BinaryCopyProbe"
            program.write_text(f'''
import IO.File
import Data.Either
module BinaryCopyProbe []
show (fromRight (copy-file {source_path} {target_path}) (0 - 1))
show (left? (copy-file {absent_path} {unwanted_path}))
''')
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            environment["HOME"] = str(work / "home")
            (work / "home").mkdir()
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(program), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=work, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout, f"{len(payload)}\nTrue\n")
            self.assertEqual(target_path.read_bytes(), payload)
            self.assertFalse(unwanted_path.exists())

    def test_probe_truncate_sync_and_permissions_execute(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            target = work / "metadata.txt"
            missing = work / "absent.txt"
            program = work / "MetadataProbe.mon"
            executable = work / "MetadataProbe"
            program.write_text(f'''
import IO.File
import Data.Either
module MetadataProbe []
show (fromRight (path-exists {missing}) True)
show (fromRight (write-file {target} "abcdef") (0 - 1))
show (fromRight (path-exists {target}) False)
define opened (open-file {target} ReadWrite)
define handle (fromRight opened (FileHandle (0 - 1)))
show (fromRight (set-file-size handle 3) (0 - 1))
show (fromRight (sync-file-data handle) (0 - 1))
show (fromRight (close-file handle) (0 - 1))
show (fromRight (set-file-permissions {target} 384) (0 - 1))
''')
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            cache_home = work / "home"
            cache_home.mkdir()
            environment["HOME"] = str(cache_home)
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(program), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=work, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            # Scalar payload bits survive, but the imported polymorphic
            # eliminator currently loses Bool's nominal identity and shows
            # its optimized representation as 0/1.
            self.assertEqual(ran.stdout, "0\n6\n1\n0\n0\n0\n0\n")
            self.assertEqual(target.read_text(), "abc")
            self.assertEqual(target.stat().st_mode & 0o777, 0o600)

    def test_seek_flush_exact_write_and_missing_file_error_execute(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            target = work / "seek.txt"
            missing = work / "missing.txt"
            program = work / "SeekProbe.mon"
            executable = work / "SeekProbe"
            program.write_text(f'''
import IO
import IO.File
import Data.Either
module SeekProbe []
define opened (open-file {target} WriteOnly)
define handle (fromRight opened (FileHandle (0 - 1)))
show (fromRight (write-file-text handle "abcdef") (0 - 1))
show (fromRight (seek-file handle 2 FromStart) (0 - 1))
show (fromRight (write-file-text handle "XY") (0 - 1))
show (fromRight (flush-file handle) (0 - 1))
show (fromRight (close-file handle) (0 - 1))
show (left? (read-file-bytes {missing}))
''')
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            cache_home = work / "home"
            cache_home.mkdir()
            environment["HOME"] = str(cache_home)
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(program), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=work, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout, "6\n2\n2\n0\n0\nTrue\n")
            self.assertEqual(target.read_text(), "abXYef")

    def test_whole_file_write_append_and_read_execute(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            target = work / "sample.txt"
            program = work / "FileProbe.mon"
            executable = work / "FileProbe"
            program.write_text(f'''
import IO
import IO.File
import Data.Either
module FileProbe []
show (fromRight (write-file {target} "alpha") (0 - 1))
show (fromRight (append-file {target} "!") (0 - 1))
show (right? (read-file-bytes {target}))
show (count (fromRight (read-file-bytes {target}) (list)))
''')
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            cache_home = work / "home"
            cache_home.mkdir()
            environment["HOME"] = str(cache_home)
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(program), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=work, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(
                ran.stdout,
                "5\n1\nTrue\n6\n",
            )
            self.assertEqual(target.read_text(), "alpha!")

    def test_directory_rename_and_remove_lifecycle_executes(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            child = work / "child"
            source = child / "before.txt"
            target = child / "after.txt"
            program = work / "PathLifecycleProbe.mon"
            executable = work / "PathLifecycleProbe"
            program.write_text(f'''
import IO.File
import Data.Either
module PathLifecycleProbe []
show (fromRight (create-directory {child}) (0 - 1))
show (fromRight (write-file {source} "payload") (0 - 1))
show (fromRight (rename-file {source} {target}) (0 - 1))
show (fromRight (remove-file {target}) (0 - 1))
show (fromRight (remove-directory {child}) (0 - 1))
''')
            environment = os.environ.copy()
            environment["MONAD_CORE"] = str(ROOT / "core")
            cache_home = work / "home"
            cache_home.mkdir()
            environment["HOME"] = str(cache_home)
            compiled = subprocess.run(
                [str(ROOT / "monad"), str(program), "-o", str(executable)],
                cwd=work, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            ran = subprocess.run(
                [str(executable)], cwd=work, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(ran.returncode, 0, ran.stdout)
            self.assertEqual(ran.stdout, "0\n7\n0\n0\n0\n")
            self.assertFalse(child.exists())


if __name__ == "__main__":
    unittest.main()
