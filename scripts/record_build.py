"""Record compiled evidence without claiming target execution or timing validation."""
import hashlib
import json
import pathlib
import os
import shlex
import subprocess

root = pathlib.Path(__file__).resolve().parents[1]
build = root / "build-arm"
tools = pathlib.Path(os.environ.get("ARM_TOOLCHAIN_ROOT", str(root / ".deps/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi"))) / "bin"
def run(*args):
    return subprocess.check_output([str(x) for x in args], text=True)
def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

commands = json.loads((build / "compile_commands.json").read_text())
for item in json.loads((root/"docs/fc-source-manifest.json").read_text(encoding="utf-8-sig"))["files"]:
    assert digest(root/".deps/CMSIS-NN"/item["path"]) == item["sha256"], "Pinned source changed"
elf = build / "fc_probe.elf"
(build / "fc_probe.dis").write_text(run(tools / "arm-none-eabi-objdump", "-d", elf))
(build / "fc_probe.symbols").write_text(run(tools / "arm-none-eabi-nm", "-n", "-S", elf))
flags = ["-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard", "-O3"]
include = root / ".deps/CMSIS-NN/Include"
macros = run(tools / "arm-none-eabi-gcc", *flags, "-I"+str(include), "-dM", "-E", root/"firmware/probe.c")
(build / "effective-macros.txt").write_text(macros)
assert "#define ARM_MATH_DSP 1" in macros
assert not any("#define "+m+" " in macros for m in ["ARM_MATH_MVEI", "CMSIS_NN_USE_SINGLE_ROUNDING", "CMSIS_NN_USE_REQUANTIZE_INLINE_ASSEMBLY"])
sources = {}
# Hash the actual preprocessor dependency closure of each translation unit.
for entry in commands:
    args = shlex.split(entry["command"])
    args = [a for a in args if a != "-c"]
    idx = args.index("-o")
    del args[idx:idx+2]
    dependencies = run(*args, "-M").replace("\\\n", " ").split(":", 1)[1].split()
    for name in dependencies:
        p = pathlib.Path(name)
        if not p.is_absolute(): p = pathlib.Path(entry["directory"])/p
        p = p.resolve()
        sources[str(p.relative_to(root)) if p.is_relative_to(root) else str(p)] = digest(p)
manifest = {
    "status": "cross-compiled analysis ELF; not board firmware; not executed on Cortex-M4",
    "host_compiler": run("g++-13", "--version").splitlines()[0],
    "cmake": run("cmake", "--version").splitlines()[0],
    "target_compiler": run(tools/"arm-none-eabi-gcc", "--version").splitlines()[0],
    "binutils": run(tools/"arm-none-eabi-objdump", "--version").splitlines()[0],
    "cmsis_commit": run("git", "-C", root/".deps/CMSIS-NN", "rev-parse", "HEAD").strip(),
    "toolchain_archive_sha256": digest(root/".deps/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi.tar.xz"),
    "linker_script_sha256": digest(root/"firmware/probe.ld"),
    "compile_commands": commands,
    "link_command": (build/"CMakeFiles/fc_probe.dir/link.txt").read_text(),
    "source_dependency_sha256": sources,
    "artifacts": {p.name:digest(p) for p in [elf, build/"fc_probe.map", build/"fc_probe.dis", build/"effective-macros.txt"]},
    "size": run(tools/"arm-none-eabi-size", elf),
    "stack_usage": {str(p.relative_to(build)):p.read_text() for p in build.rglob("*.su")},
}
(build/"manifest.json").write_text(json.dumps(manifest, indent=2)+"\n")
print(manifest["target_compiler"])
print(manifest["size"])
print("Wrote build-arm/manifest.json, disassembly, symbols and effective macros")
