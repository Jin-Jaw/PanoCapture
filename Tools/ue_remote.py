"""Run Python in the open Unreal Editor through the Python plugin's remote execution (localhost only).

Needs PythonScriptPlugin enabled in the host project and, in its Config/DefaultEngine.ini:

    [/Script/PythonScriptPlugin.PythonScriptPluginSettings]
    bRemoteExecution=True

Usage (set UE_ENGINE_DIR when the engine isn't at G:\\UE_5.6):

    python Tools/ue_remote.py "print(unreal.SystemLibrary.get_engine_version())"
    python Tools/ue_remote.py --file some_script.py
    python Tools/ue_remote.py --livecoding        # same as Ctrl+Alt+F11 in the editor

For example, capture the tour on the level's camera:

    python Tools/ue_remote.py "[a for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if isinstance(a, unreal.PanoCaptureCamera)][0].capture_tour()"
"""

import argparse
import os
import sys
import time

ENGINE_DIR = os.environ.get("UE_ENGINE_DIR", r"G:\UE_5.6")
sys.path.append(os.path.join(ENGINE_DIR, "Engine", "Plugins", "Experimental", "PythonScriptPlugin", "Content", "Python"))
import remote_execution  # noqa: E402


def run(command, timeout=10.0, exec_mode=remote_execution.MODE_EXEC_FILE):
    remote = remote_execution.RemoteExecution()
    remote.start()
    try:
        deadline = time.time() + timeout
        while not remote.remote_nodes and time.time() < deadline:
            time.sleep(0.1)
        if not remote.remote_nodes:
            print("No Unreal Editor found. Is it open with Python remote execution enabled?", file=sys.stderr)
            return 2

        remote.open_command_connection(remote.remote_nodes[0])
        result = remote.run_command(command, unattended=True, exec_mode=exec_mode)
        for line in result.get("output", []):
            print(line.get("output", "").rstrip())
        if not result.get("success", False):
            print(result.get("result", ""), file=sys.stderr)
            return 1
        return 0
    finally:
        remote.stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("code", nargs="?", help="Python to run in the editor")
    parser.add_argument("--file", help="Python file to run in the editor")
    parser.add_argument("--livecoding", action="store_true", help="Trigger a Live Coding compile")
    args = parser.parse_args()

    if args.livecoding:
        code = "import unreal\nunreal.SystemLibrary.execute_console_command(None, 'LiveCoding.Compile')"
    elif args.file:
        with open(args.file, encoding="utf-8") as f:
            code = f.read()
    elif args.code:
        code = "import unreal\n" + args.code
    else:
        parser.error("pass code, --file or --livecoding")

    sys.exit(run(code))


if __name__ == "__main__":
    main()
