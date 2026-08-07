if(NOT DEFINED FOREVERTAS_SOURCE_DIR)
    message(FATAL_ERROR "FOREVERTAS_SOURCE_DIR is required")
endif()

function(require_text text needle diagnostic)
    string(FIND "${text}" "${needle}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${diagnostic}: missing `${needle}`")
    endif()
endfunction()

file(READ
    "${FOREVERTAS_SOURCE_DIR}/packaging/release/package-linux.sh"
    linux_script)
file(READ
    "${FOREVERTAS_SOURCE_DIR}/packaging/release/package-windows.ps1"
    windows_script)
file(READ
    "${FOREVERTAS_SOURCE_DIR}/packaging/release/build-linux-local.sh"
    linux_build_script)
file(READ
    "${FOREVERTAS_SOURCE_DIR}/packaging/release/build-windows-local.ps1"
    windows_build_script)
file(READ
    "${FOREVERTAS_SOURCE_DIR}/packaging/release/local-release.py"
    release_driver)
file(READ
    "${FOREVERTAS_SOURCE_DIR}/packaging/release/manifest.json"
    release_manifest)
file(READ
    "${FOREVERTAS_SOURCE_DIR}/packaging/release/cuda_compiler_launcher.py"
    cuda_launcher)
file(READ
    "${FOREVERTAS_SOURCE_DIR}/packaging/windows/test-portable.ps1"
    windows_portable_test)
file(READ
    "${FOREVERTAS_SOURCE_DIR}/packaging/linux/build-appimage.sh"
    appimage_script)

set(expected_architectures
    "61-real;62-real;70-real;72-real;75-real;80-real;86-real;87-real;89-real;90-real;100-real;101-real;120-real;120-virtual")
set(expected_architecture_key
    "sm61-sm62-sm70-sm72-sm75-sm80-sm86-sm87-sm89-sm90-sm100-sm101-sm120-ptx120")
set(expected_cubins
    "61, 62, 70, 72, 75, 80, 86, 87, 89, 90, 100, 101, 120")

foreach(platform IN ITEMS linux windows)
    set(script "${${platform}_script}")
    require_text("${script}" "cuda-search-object-v2"
        "${platform} CUDA search cache schema")
    foreach(field IN ITEMS
            "cache_schema="
            "toolchain="
            "cuda="
            "architectures="
            "architecture_key="
            "split_compile_jobs="
            "validator=")
        require_text("${script}" "${field}"
            "${platform} CUDA search cache identity")
    endforeach()
    require_text("${script}" "${expected_architectures}"
        "${platform} exact CUDA architecture tuple")
    require_text("${script}" "${expected_architecture_key}"
        "${platform} exact CUDA architecture key")
    require_text("${script}"
        "forevervalidator_cuda_search.ltoir"
        "${platform} session LTO cache-hit/miss validation")
    require_text("${script}"
        "forevervalidator_cuda_search_lto_ir.h"
        "${platform} embedded session LTO validation")
    require_text("${script}" "ForeverValidatorCudaSearchLtoIr"
        "${platform} embedded session LTO symbol validation")
    if(platform STREQUAL "linux")
        require_text("${script}" "verify_session_lto_artifacts"
            "Linux session LTO validation is shared by cache paths")
    else()
        require_text("${script}" "Test-CudaSessionLtoArtifacts"
            "Windows session LTO validation is shared by cache paths")
    endif()
    if(script MATCHES "Get-FileHash[^\n]*CMakeLists.txt" OR
       script MATCHES "sha256sum[^\n]*CMakeLists.txt")
        message(FATAL_ERROR
            "${platform} CUDA search cache depends on unrelated CMake edits")
    endif()
endforeach()

# The session-specialized LTO check must remain after the cache-miss publish
# branch so the same mandatory validation covers both cold and warm builds.
string(FIND "${linux_script}"
    "if [[ \"\${cache_hit}\" == false ]]" linux_cache_miss_position)
string(FIND "${linux_script}"
    "if ! verify_session_lto_artifacts" linux_lto_check_position)
if(linux_cache_miss_position EQUAL -1 OR
   linux_lto_check_position LESS_EQUAL linux_cache_miss_position)
    message(FATAL_ERROR
        "Linux LTO validation is not shared by cache-hit and cache-miss paths")
endif()
string(FIND "${windows_script}"
    "if (-not $CacheHit)" windows_cache_miss_position)
string(FIND "${windows_script}"
    "if (-not (Test-CudaSessionLtoArtifacts $ValidatorBuildDirectory))"
    windows_lto_check_position)
if(windows_cache_miss_position EQUAL -1 OR
   windows_lto_check_position LESS_EQUAL windows_cache_miss_position)
    message(FATAL_ERROR
        "Windows LTO validation is not shared by cache-hit and cache-miss paths")
endif()

require_text("${linux_script}" "cmp -s <(printf '%s\\n' \"\${cache_identity[@]}\")"
    "Linux cache reads the same schema-v2 metadata that it writes")
require_text("${linux_script}" "printf '%s\\n' \"\${cache_identity[@]}\""
    "Linux cache miss writes the canonical metadata identity")
require_text("${linux_script}" "grep -oE 'sm_[0-9]+\\.cubin'"
    "Linux exact cubin-set parser")
require_text("${linux_script}" "grep -oE 'sm_[0-9]+\\.ptx'"
    "Linux exact PTX-set parser")
require_text("${windows_script}" "Compare-Object $ActualCubins $ExpectedCubinArchitectures"
    "Windows exact cubin-set comparison")
require_text("${windows_script}" "Compare-Object $ActualPtx $ExpectedPtxArchitectures"
    "Windows exact PTX-set comparison")
require_text("${windows_script}" "$CacheIdentity |"
    "Windows cache miss writes the canonical metadata identity")

foreach(build_script IN ITEMS linux_build_script windows_build_script)
    require_text("${${build_script}}" "12.8.1"
        "release wrapper CUDA version guard")
    require_text("${${build_script}}" "${expected_architectures}"
        "release wrapper architecture guard")
    require_text("${${build_script}}" "${expected_architecture_key}"
        "release wrapper architecture-key guard")
    require_text("${${build_script}}" "search_object_source_commit"
        "release wrapper exact Validator source guard")
endforeach()

require_text("${release_manifest}" "[${expected_cubins}]"
    "release manifest exact native cubin list")
require_text("${release_manifest}" "\"ptx_architecture\": 120"
    "release manifest PTX fallback")
require_text("${release_manifest}" "${expected_architectures}"
    "release manifest exact CMake architecture tuple")
require_text("${release_manifest}" "${expected_architecture_key}"
    "release manifest architecture key")
if(release_manifest MATCHES "\"tag\"[ \t]*:[ \t]*\"v0\\.2\\.2\"[^\n]*forevervalidator" OR
   release_manifest MATCHES "search_object_output_neutral_paths")
    message(FATAL_ERROR
        "release manifest retained a stale Validator tag or output-neutral exception")
endif()

require_text("${release_driver}" "stage_manifest(manifest_path, destination)"
    "prepared release trees stage the selected manifest")
require_text("${release_driver}" "tree / \"packaging/release/manifest.json\""
    "Linux release consumes its staged manifest")
require_text("${release_driver}" "remote}/packaging/release/manifest.json"
    "Windows release consumes its staged manifest")
require_text("${release_driver}" "optional tag is stale"
    "optional Validator tags fail closed")

foreach(runtime_pattern IN ITEMS
        "cudart64_*.dll"
        "nvrtc64_120_0.dll"
        "nvrtc-builtins64_*.dll"
        "nvJitLink_*.dll")
    require_text("${windows_portable_test}" "${runtime_pattern}"
        "Windows portable CUDA runtime closure")
endforeach()
require_text("${windows_portable_test}" "RuntimeMatches.Count -ne 1"
    "Windows portable CUDA runtime ambiguity check")
require_text("${windows_portable_test}" "[switch]$RequireCuda"
    "generic Windows portable checks keep CUDA closure opt-in")
require_text("${windows_script}" "-Archive $Artifact.FullName -RequireCuda"
    "Windows CUDA release enables portable CUDA runtime closure")
require_text("${appimage_script}" "libnvrtc.so*"
    "AppImage NVRTC runtime check")
require_text("${appimage_script}" "libnvJitLink.so*"
    "AppImage nvJitLink runtime check")
require_text("${appimage_script}" "-name 'libcuda.so*'"
    "AppImage host-driver rejection")

require_text("${cuda_launcher}" "produces_lto_ir"
    "session LTO remains enabled beside AOT object caching")
require_text("${cuda_launcher}" "compiles_search_executor and not produces_lto_ir"
    "only the generic AOT search object bypasses sccache")

string(TOLOWER
    "${release_manifest}\n${release_driver}\n${linux_script}\n${windows_script}\n${linux_build_script}\n${windows_build_script}"
    release_contract_text)
foreach(forbidden IN ITEMS
        "search_object_output_neutral_paths"
        "output.neutral"
        "empty.air"
        "profil"
        "experimental")
    if(release_contract_text MATCHES "${forbidden}")
        message(FATAL_ERROR
            "release cache retained forbidden experimental marker: ${forbidden}")
    endif()
endforeach()
string(REGEX MATCHALL
    "forevervalidator_cuda_search_prebuilt_[a-z0-9_]+"
    prebuilt_markers "${release_contract_text}")
foreach(marker IN LISTS prebuilt_markers)
    if(NOT marker STREQUAL
            "forevervalidator_cuda_search_prebuilt_object")
        message(FATAL_ERROR
            "release cache retained forbidden prebuilt feature marker: ${marker}")
    endif()
endforeach()
if(release_contract_text MATCHES "50-real" OR
   release_contract_text MATCHES "52-real" OR
   release_contract_text MATCHES "sm50" OR
   release_contract_text MATCHES "sm52")
    message(FATAL_ERROR "release cache retained obsolete sm50/sm52 images")
endif()

find_program(Python3_EXECUTABLE NAMES python3 python REQUIRED)
set(contract_test [=[
from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


driver_path = Path(sys.argv[1]).resolve()
manifest_path = Path(sys.argv[2]).resolve()
spec = importlib.util.spec_from_file_location("forevertas_local_release", driver_path)
driver = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(driver)


def expect_failure(action, diagnostic: str) -> None:
    try:
        action()
    except SystemExit as error:
        if diagnostic not in str(error):
            raise AssertionError(
                f"expected diagnostic {diagnostic!r}, received {str(error)!r}")
    else:
        raise AssertionError(f"expected failure containing {diagnostic!r}")


manifest = driver.load_manifest(manifest_path)
commit = manifest["sources"]["forevervalidator"]["commit"]
assert "tag" not in manifest["sources"]["forevervalidator"]
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)

    def write_manifest(value: dict, name: str) -> Path:
        path = root / name
        path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
        return path

    mismatch = copy.deepcopy(manifest)
    mismatch["cuda"]["search_object_source_commit"] = "f" * 40
    expect_failure(
        lambda: driver.load_manifest(write_manifest(mismatch, "mismatch.json")),
        "must equal the ForeverValidator pin",
    )

    uppercase = copy.deepcopy(manifest)
    uppercase["sources"]["forevervalidator"]["commit"] = commit.upper()
    uppercase["cuda"]["search_object_source_commit"] = commit.upper()
    expect_failure(
        lambda: driver.load_manifest(write_manifest(uppercase, "uppercase.json")),
        "valid ForeverValidator commit SHA",
    )

    wrong_arches = copy.deepcopy(manifest)
    wrong_arches["cuda"]["architectures"] = [50, *wrong_arches["cuda"]["architectures"]]
    expect_failure(
        lambda: driver.load_manifest(write_manifest(wrong_arches, "arches.json")),
        "architecture list",
    )

    for marker in (
        "search_object_output_neutral_paths",
        "empty_air_certificate",
        "profiling_marker",
        "prebuilt_feature",
    ):
        marked = copy.deepcopy(manifest)
        marked["cuda"][marker] = []
        expect_failure(
            lambda marked=marked, marker=marker: driver.load_manifest(
                write_manifest(marked, f"{marker}.json")),
            "CUDA has unsupported fields",
        )

    selected_bytes = json.dumps(manifest, indent=1).encode("utf-8") + b"\n\n"
    selected = root / "selected.json"
    selected.write_bytes(selected_bytes)
    driver.export_source = lambda source, revision, destination: destination.mkdir(
        parents=True, exist_ok=True)
    state = {
        "forevertas": "1" * 40,
        "forevervalidator": commit,
        "version": manifest["release"]["version"],
    }
    fake_validator = root / "validator"
    for platform in ("linux", "windows"):
        destination = root / f"{platform}-source"
        driver.prepare_tree(selected, fake_validator, state, destination)
        staged = destination / "packaging" / "release" / "manifest.json"
        assert staged.read_bytes() == selected_bytes

    fake_tas = root / "tas"
    fake_tas.mkdir()
    fake_validator.mkdir(exist_ok=True)
    (fake_validator / "CMakeLists.txt").write_text(
        "project(ForeverValidator VERSION 0.2.2 LANGUAGES CXX)\n",
        encoding="utf-8",
    )
    (fake_validator / "vcpkg.json").write_text(
        json.dumps({"version-string": "0.2.2"}), encoding="utf-8")
    driver.REPO_ROOT = fake_tas

    tag_target = commit

    def fake_git(*arguments: str, cwd: Path | None = None) -> str:
        if arguments[:2] == ("rev-parse", "HEAD"):
            return commit
        if arguments and arguments[0] == "cat-file":
            return ""
        if arguments and arguments[0] == "rev-parse" and arguments[1].endswith("^{}"):
            return tag_target
        raise AssertionError(f"unexpected git invocation: {arguments!r}")

    driver.git = fake_git
    valid_cmake = (
        "project(ForeverTAS\n    VERSION 0.2.2\n)\n"
        "FetchContent_Declare(\n"
        "    ForeverValidator\n"
        f"    GIT_TAG {commit})\n"
    )
    (fake_tas / "CMakeLists.txt").write_text(valid_cmake, encoding="utf-8")
    driver.source_state(manifest, fake_validator)

    wrong_cmake = (
        valid_cmake.replace(commit, "e" * 40) +
        f"# shadow GIT_TAG {commit}\n" +
        f"FetchContent_Declare(OtherDependency GIT_TAG {commit})\n"
    )
    (fake_tas / "CMakeLists.txt").write_text(wrong_cmake, encoding="utf-8")
    expect_failure(
        lambda: driver.source_state(manifest, fake_validator),
        "CMake ForeverValidator pin",
    )
    (fake_tas / "CMakeLists.txt").write_text(valid_cmake, encoding="utf-8")

    tagged = copy.deepcopy(manifest)
    tagged["sources"]["forevervalidator"]["tag"] = "stale-validator-tag"
    tag_target = "e" * 40
    expect_failure(
        lambda: driver.source_state(tagged, fake_validator),
        "optional tag is stale",
    )

    package_linux = (driver_path.parent / "package-linux.sh").read_text(
        encoding="utf-8")
    assignments = []
    for name in ("expected_cubin_architectures", "expected_ptx_architecture"):
        match = re.search(rf'^{name}="[^"]+"$', package_linux, re.MULTILINE)
        assert match is not None
        assignments.append(match.group(0))
    def shell_function(name: str) -> str:
        match = re.search(
            rf"^{name}\(\) \{{\n.*?^\}}\n",
            package_linux,
            re.MULTILINE | re.DOTALL,
        )
        assert match is not None
        return match.group(0)

    architecture_verifier = shell_function("verify_architectures")
    cache_verifier = shell_function("verify_cache_integrity")
    lto_verifier = shell_function("verify_session_lto_artifacts")
    bash = shutil.which("bash")
    if os.name == "nt":
        git_bash = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / \
            "Git" / "bin" / "bash.exe"
        if git_bash.is_file():
            bash = str(git_bash)
    assert bash is not None
    toolkit = root / "fake-cuda"
    (toolkit / "bin").mkdir(parents=True)
    cuobjdump = toolkit / "bin" / "cuobjdump"
    cuobjdump.write_text(
        "#!/usr/bin/env bash\n"
        "if [[ \"$1\" == \"--list-elf\" ]]; then\n"
        "    printf '%s\\n' \"${MOCK_CUDA_ELF_OUTPUT}\"\n"
        "else\n"
        "    printf '%s\\n' \"${MOCK_CUDA_PTX_OUTPUT}\"\n"
        "fi\n",
        encoding="utf-8",
    )
    cuobjdump.chmod(0o755)
    object_path = root / "cuda-object.o"
    object_path.write_bytes(b"object")
    verifier_path = root / "verify-architectures.sh"
    verifier_path.write_text(
        "#!/usr/bin/env bash\nset -euo pipefail\n" +
        "\n".join(assignments) + "\n" + architecture_verifier +
        'CUDA_PATH="$1"\nverify_architectures "$2"\n',
        encoding="utf-8",
    )
    verifier_path.chmod(0o755)

    cubins = [61, 62, 70, 72, 75, 80, 86, 87, 89, 90, 100, 101, 120]
    exact_elf = "\n".join(f"sm_{value}.cubin" for value in cubins)
    exact_ptx = "sm_120.ptx"

    def architecture_result(elf: str, ptx: str) -> int:
        environment = os.environ.copy()
        environment["MOCK_CUDA_ELF_OUTPUT"] = elf
        environment["MOCK_CUDA_PTX_OUTPUT"] = ptx
        return subprocess.run(
            [bash, verifier_path.as_posix(), toolkit.as_posix(),
             object_path.as_posix()],
            env=environment,
            check=False,
        ).returncode

    assert architecture_result(exact_elf, exact_ptx) == 0
    assert architecture_result("sm_50.cubin\n" + exact_elf, exact_ptx) != 0
    assert architecture_result(exact_elf.replace("sm_87.cubin\n", ""), exact_ptx) != 0
    assert architecture_result(exact_elf, exact_ptx + "\nsm_121.ptx") != 0

    cache_identity = [
        "cache_schema=cuda-search-object-v2",
        "toolchain=mock-toolchain",
        "cuda=12.8.1",
        f"architectures={manifest['cuda']['cmake_architectures']}",
        f"architecture_key={manifest['cuda']['architecture_key']}",
        "split_compile_jobs=4",
        f"validator={commit}",
    ]
    cache_verifier_path = root / "verify-cache.sh"
    cache_verifier_path.write_text(
        "#!/usr/bin/env bash\nset -euo pipefail\n" +
        "\n".join(assignments) + "\ncache_identity=(\n" +
        "\n".join(f'    "{line}"' for line in cache_identity) +
        "\n)\n" + architecture_verifier + cache_verifier +
        'CUDA_PATH="$1"\nverify_cache_integrity "$2"\n',
        encoding="utf-8",
    )
    cache_verifier_path.chmod(0o755)
    cache_directory = root / "warm-cache"
    cache_directory.mkdir()
    cached_object = cache_directory / "cuda_search_executor.cu.o"
    cached_bytes = b"cached CUDA object"
    metadata_path = cache_directory / "metadata.txt"

    def cache_result(elf: str = exact_elf, ptx: str = exact_ptx):
        environment = os.environ.copy()
        environment["MOCK_CUDA_ELF_OUTPUT"] = elf
        environment["MOCK_CUDA_PTX_OUTPUT"] = ptx
        return subprocess.run(
            [bash, cache_verifier_path.as_posix(), toolkit.as_posix(),
             cache_directory.as_posix()],
            env=environment,
            check=False,
            capture_output=True,
            text=True,
        )

    # A missing entry is a cache miss; the same identity becomes a hit only
    # after the object, closed metadata, and digest have all been published.
    assert cache_result().returncode != 0
    cached_object.write_bytes(cached_bytes)
    metadata_path.write_bytes(("\n".join(cache_identity) + "\n").encode())
    (cache_directory / "object.sha256").write_bytes(
        (f"{hashlib.sha256(cached_bytes).hexdigest()}  "
         f"{cached_object.name}\n").encode("ascii"))
    warm_result = cache_result()
    assert warm_result.returncode == 0, (warm_result.stdout, warm_result.stderr)
    metadata_path.write_bytes(
        ("\n".join([*cache_identity, "profiling_marker=on"]) + "\n").encode())
    assert cache_result().returncode != 0
    metadata_path.write_bytes(("\n".join(cache_identity) + "\n").encode())
    cached_object.write_bytes(cached_bytes + b"corrupt")
    assert cache_result().returncode != 0
    cached_object.write_bytes(cached_bytes)
    assert cache_result("sm_50.cubin\n" + exact_elf).returncode != 0

    lto_verifier_path = root / "verify-lto.sh"
    lto_verifier_path.write_text(
        "#!/usr/bin/env bash\nset -euo pipefail\n" + lto_verifier +
        'verify_session_lto_artifacts "$1"\n',
        encoding="utf-8",
    )
    lto_verifier_path.chmod(0o755)
    generated = root / "generated"
    generated.mkdir()
    lto_ir = generated / "forevervalidator_cuda_search.ltoir"
    lto_header = generated / "forevervalidator_cuda_search_lto_ir.h"
    lto_ir.write_bytes(b"LTO IR")
    lto_header.write_text(
        "unsigned char ForeverValidatorCudaSearchLtoIr[] = {1};\n",
        encoding="ascii")

    def lto_result() -> int:
        return subprocess.run(
            [bash, lto_verifier_path.as_posix(), generated.as_posix()],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ).returncode

    assert lto_result() == 0
    lto_ir.write_bytes(b"")
    assert lto_result() != 0
    lto_ir.write_bytes(b"LTO IR")
    lto_header.write_text("wrong symbol\n", encoding="ascii")
    assert lto_result() != 0

print("PASS release manifest, source identity, and staged-manifest controls")
]=])
set(contract_test_path
    "${CMAKE_CURRENT_BINARY_DIR}/release-cache-contract-test.py")
file(WRITE "${contract_test_path}" "${contract_test}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${contract_test_path}"
        "${FOREVERTAS_SOURCE_DIR}/packaging/release/local-release.py"
        "${FOREVERTAS_SOURCE_DIR}/packaging/release/manifest.json"
    RESULT_VARIABLE contract_result
    OUTPUT_VARIABLE contract_output
    ERROR_VARIABLE contract_error)
if(NOT contract_result EQUAL 0)
    message(FATAL_ERROR
        "release contract Python checks failed (${contract_result})\n"
        "${contract_output}${contract_error}")
endif()
message(STATUS "${contract_output}")
