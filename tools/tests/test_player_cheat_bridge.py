"""Exercise the production Extras/command/pad bridge and script lifecycle hooks.

The core's pure PlayerOptions implementation is reused; guest actors, settings
storage and command transport are authored boundaries. No game is executed.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def extract(source, pattern):
    match = re.search(pattern, source, re.M | re.S)
    if not match:
        raise RuntimeError("production definition not found: " + pattern)
    return match.group()


def function(source, signature):
    start = re.search("^" + re.escape(signature) + r"[^;{]*\{", source, re.M)
    if not start:
        raise RuntimeError("production function not found: " + signature)
    brace = start.end() - 1
    depth = 0
    tokens = r'''"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|//[^\n]*|/\*.*?\*/|[{}]'''
    for token in re.finditer(tokens, source[brace:], re.S):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start.start():brace + token.end()]
    raise RuntimeError("unclosed production function: " + signature)


def define(name, arguments, body, result):
    parts = name.split("::")
    declaration = f"{result} {parts[-1]}({arguments}) {{ {body} }}"
    return "namespace " + "::".join(parts[:-1]) + " { " + declaration + " }" if len(parts) > 1 else declaration


def main():
    extras = (ROOT / "games/bo2/src/bo2zm_extras.cpp").read_text(encoding="utf-8")
    commands = (ROOT / "games/bo2/src/bo2zm_commands.cpp").read_text(encoding="utf-8")
    pad = (ROOT / "games/bo2/src/bo2zm_pad.cpp").read_text(encoding="utf-8")
    gsc = (ROOT / "games/bo2/src/bo2zm_gsc_mods.cpp").read_text(encoding="utf-8")
    core = ROOT / "games/bo2/src/bo2zm_player_cheat_state.h"
    if not core.is_file():
        core = ROOT.parent / "player-cheats/games/bo2/src/bo2zm_player_cheat_state.h"
    if not core.is_file():
        raise SystemExit("integrate the player-cheats core or provide its sibling worktree first")

    table = extract(extras, r"^constexpr Option kOptions\[\] = \{.*?^};")
    rows = re.findall(r'\{\s*"([^"]+)"\s*,\s*(?:nullptr|"[^"]*")\s*,\s*\w+\s*,\s*'
                      r'(?:int\(std::size\(kRounds\)\)|\d+)\s*,\s*([\w:]+)\s*,\s*([\w:]+)\s*}', table)
    if len(rows) != table.count('{"'):
        raise RuntimeError("fixture did not consume the entire real option table")
    boundaries = ["void SetCheats(int);"]
    for name, getter, setter in rows:
        if getter != "RoundChoice":
            boundaries.append(define(getter, "", f'return LegacyGet("{name}");', "int"))
        if setter not in ("SetCheats", "SetRoundChoice"):
            boundaries.append(define(setter, "int value", f'LegacySet("{name}", value);', "void"))
    fragments = re.findall(r"^constexpr const char\* \w+\[\] = .*?;", extras, re.M | re.S)
    fragments += [extract(extras, r"^int g_round_choice = .*?;"),
                  extract(extras, r"^std::atomic<bool> g_changed.*?;"),
                  extract(extras, r"^constexpr int kCompareEveryFrames = .*?;")]
    for name in ("RoundChoice", "SetRoundChoice", "IsCheat", "IsPlayerOption", "IsWorldCommand", "IsPlayerAction",
                 "ValidLocal", "PrimaryLocal", "PlayerMaster"):
        fragments.append(function(extras, ("void " if name == "SetRoundChoice" else
                                         "int " if name in ("RoundChoice", "PlayerMaster") else "bool ") + name + "("))
    fragments += [extract(extras, r"^struct Option \{.*?^};"), table,
                  extract(extras, r"^constexpr size_t kCount = .*?;"),
                  extract(extras, r"^std::array<int, kCount> g_parked = .*?^}\(\);")]
    for signature in ("void SetCheats(", "int PlayerValue(", "bool SetPlayerValue(", "int Parse(",
                      "std::string PlayerPreference(", "void LoadPlayerPreferences(", "void AppendUi(",
                      "bool bo2zm::extras::RunCommand(", "bool bo2zm::extras::RunPlayerCommand(",
                      "void bo2zm::extras::EveryFrame("):
        fragments.append(function(extras, signature))
    command_code = commands[commands.index("constexpr uint32_t kMaxText"):commands.index("}  // namespace")]
    command_code += "\n" + function(commands, "REX_HOOK_RAW(sub_82402E78)")
    pad_code = extract(pad, r"^constexpr uint8_t kTriggerHeld = .*?;") + "\n"
    pad_code += function(pad, "REX_HOOK_RAW(sub_827D8A48)")
    lifecycle = "\n".join(function(gsc, "REX_HOOK_RAW(" + name + ")")
                          for name in ("sub_8252DED0", "sub_823599E8"))
    fixture = (ROOT / "tools/tests/player_cheat_bridge_test.cpp").read_text(encoding="utf-8")
    replacements = {"// CORE_STATE_HEADER": '#include "' + core.as_posix() + '"',
                    "// BOT_REGISTRATION_CONSTANTS": "\n".join(extract(gsc, r"^constexpr uint32_t " + name + r" = .*?;")
                                                                         for name in ("kRegisterInt", "kSetInt")),
                    "// PRODUCTION_BOT_COUNT_CODE": function(gsc, "bool SetBotCount("),
                    "// PRODUCTION_BOT_REQUEST": extract(gsc, r"^  const int requested = .*?;"),
                    "// OPTION_BOUNDARY_FUNCTIONS": "\n".join(boundaries),
                    "// PRODUCTION_EXTRAS_CODE": "\n\n".join(fragments),
                    "// PRODUCTION_COMMANDS_CODE": command_code, "// PRODUCTION_PAD_CODE": pad_code,
                    "// PRODUCTION_LIFECYCLE_CODE": lifecycle,
                    '../../games/bo2/src/bo2zm_pad.h': (ROOT / "games/bo2/src/bo2zm_pad.h").as_posix()}
    replacements['../../games/bo2/src/bo2zm_bots.h'] = (ROOT / "games/bo2/src/bo2zm_bots.h").as_posix()
    for marker, value in replacements.items():
        fixture = fixture.replace(marker, value)
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        raise SystemExit("set CXX to a C++23 compiler")
    with tempfile.TemporaryDirectory(prefix="bo2-player-cheat-bridge-") as directory:
        cpp, exe = Path(directory) / "bridge.cpp", Path(directory) / ("bridge.exe" if os.name == "nt" else "bridge")
        cpp.write_text(fixture, encoding="utf-8")
        includes = [ROOT / "sdk/include", ROOT / "sdk/thirdparty/simde", ROOT / "sdk/thirdparty/fmt/include",
                    ROOT / "sdk/thirdparty/spdlog/include"]
        env = dict(os.environ)
        env["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror", "-msse4.1",
                        "-D_CRT_SECURE_NO_WARNINGS", *["-I" + str(p) for p in includes],
                        str(cpp), "-o", str(exe)], check=True, env=env)
        for scenario in ("dispatch", "preferences", "pad", "lifecycle", "bots"):
            subprocess.run([str(exe), scenario], check=True, env=env)


if __name__ == "__main__":
    main()
