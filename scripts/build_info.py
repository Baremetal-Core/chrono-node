# Names build artifacts chrono-node-fw.* and embeds the git revision.
import subprocess

Import("env")

env.Replace(PROGNAME="chrono-node-fw")


def git_revision():
    try:
        return (
            subprocess.check_output(
                ["git", "describe", "--always", "--dirty", "--exclude", "*"],
                cwd=env.subst("$PROJECT_DIR"),
                stderr=subprocess.DEVNULL,
            )
            .decode()
            .strip()
        )
    except Exception:
        return "unknown"


GIT_REV_DEFINE = ("CHRONO_GIT_REV", '\\"%s\\"' % git_revision())


# Only project sources get the define, so a new revision doesn't rebuild the framework.
def add_revision(env, node):
    return env.Object(node, CPPDEFINES=list(env["CPPDEFINES"]) + [GIT_REV_DEFINE])


env.AddBuildMiddleware(add_revision, "*/src/*.cpp")
