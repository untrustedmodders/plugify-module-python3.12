import contextlib
import dataclasses
import json
import os
import subprocess
from typing import Any

from test import support

from .utils import (
    StrPath, StrJSON, TestTuple, TestFilter, FilterTuple, FilterDict)


class JsonFileType:
    UNIX_FD = "UNIX_FD"
    WINDOWS_HANDLE = "WINDOWS_HANDLE"
    STDOUT = "STDOUT"


@dataclasses.dataclass(slots=True, frozen=True)
class JsonFile:
    # file type depends on file_type:
    # - UNIX_FD: file descriptor (int)
    # - WINDOWS_HANDLE: handle (int)
    # - STDOUT: use process stdout (None)
    file: int | None
    file_type: str

    def configure_subprocess(self, popen_kwargs: dict) -> None:
        match self.file_type:
            case JsonFileType.UNIX_FD:
                # Unix file descriptor
                popen_kwargs['pass_fds'] = [self.file]
            case JsonFileType.WINDOWS_HANDLE:
                # Windows handle
                # We run mypy with `--platform=linux-x86_64` so it complains about this:
                startupinfo = subprocess.STARTUPINFO()  # type: ignore[attr-defined]
                startupinfo.lpAttributeList = {"handle_list": [self.file]}
                popen_kwargs['startupinfo'] = startupinfo

    @contextlib.contextmanager
    def inherit_subprocess(self):
        if self.file_type == JsonFileType.WINDOWS_HANDLE:
            os.set_handle_inheritable(self.file, True)
            try:
                yield
            finally:
                os.set_handle_inheritable(self.file, False)
        else:
            yield

    def open(self, mode='r', *, encoding):
        if self.file_type == JsonFileType.STDOUT:
            raise ValueError("for STDOUT file type, just use sys.stdout")

        file = self.file
        if self.file_type == JsonFileType.WINDOWS_HANDLE:
            import msvcrt
            # Create a file descriptor from the handle
            file = msvcrt.open_osfhandle(file, os.O_WRONLY)
        return open(file, mode, encoding=encoding)


@dataclasses.dataclass(slots=True, frozen=True)
class HuntRefleak:
    warmups: int
    runs: int
    filename: StrPath


@dataclasses.dataclass(slots=True, frozen=True)
class RunTests:
    tests: TestTuple
    fail_fast: bool
    fail_env_changed: bool
    match_tests: TestFilter
    match_tests_dict: FilterDict | None
    rerun: bool
    forever: bool
    pgo: bool
    pgo_extended: bool
    output_on_failure: bool
    timeout: float | None
    verbose: int
    quiet: bool
    hunt_refleak: HuntRefleak | None
    test_dir: StrPath | None
    use_junit: bool
    memory_limit: str | None
    gc_threshold: int | None
    use_resources: tuple[str, ...]
    python_cmd: tuple[str, ...] | None
    randomize: bool
    random_seed: int | str

    def copy(self, **override) -> 'RunTests':
        state = dataclasses.asdict(self)
        state.update(override)
        return RunTests(**state)

    def create_worker_runtests(self, **override):
        state = dataclasses.asdict(self)
        state.update(override)
        return WorkerRunTests(**state)

    def get_match_tests(self, test_name) -> FilterTuple | None:
        if self.match_tests_dict is not None:
            return self.match_tests_dict.get(test_name, None)
        else:
            return None

    def get_jobs(self):
        # Number of run_single_test() calls needed to run all tests.
        # None means that there is not bound limit (--forever option).
        if self.forever:
            return None
        return len(self.tests)

    def iter_tests(self):
        if self.forever:
            while True:
                yield from self.tests
        else:
            yield from self.tests

    def json_file_use_stdout(self) -> bool:
        # Use STDOUT in two cases:
        #
        # - If --python command line option is used;
        # - On Emscripten and WASI.
        #
        # On other platforms, UNIX_FD or WINDOWS_HANDLE can be used.
        return (
            bool(self.python_cmd)
            or support.is_emscripten
            or support.is_wasi
        )


@dataclasses.dataclass(slots=True, frozen=True)
class WorkerRunTests(RunTests):
    json_file: JsonFile

    def as_json(self) -> StrJSON:
        return json.dumps(self, cls=_EncodeRunTests)

    @staticmethod
    def from_json(worker_json: StrJSON) -> 'WorkerRunTests':
        return json.loads(worker_json, object_hook=_decode_runtests)


class _EncodeRunTests(json.JSONEncoder):
    def default(self, o: Any) -> dict[str, Any]:
        if isinstance(o, WorkerRunTests):
            result = dataclasses.asdict(o)
            result["__runtests__"] = True
            return result
        else:
            return super().default(o)


def _decode_runtests(data: dict[str, Any]) -> RunTests | dict[str, Any]:
    if "__runtests__" in data:
        data.pop('__runtests__')
        if data['hunt_refleak']:
            data['hunt_refleak'] = HuntRefleak(**data['hunt_refleak'])
        if data['json_file']:
            data['json_file'] = JsonFile(**data['json_file'])
        return WorkerRunTests(**data)
    else:
        return data
