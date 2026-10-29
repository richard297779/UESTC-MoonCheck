"""Keep local usernames and checkout paths out of __FILE__ and debug metadata."""
from pathlib import Path

Import('env')

# Applies to project sources and rebuilt Arduino libraries, without editing SDKs
# or rewriting already-built firmware bytes. GCC accepts forward-slash paths.
env.Append(CCFLAGS=[
    '-ffile-prefix-map=' + Path.home().as_posix() + '=/build/home',
    '-ffile-prefix-map=' + Path(env.subst('$PROJECT_DIR')).parent.as_posix() + '=/src/MoonCheck',
])
