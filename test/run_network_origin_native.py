"""Run internal native seam tests against the actual configured Release objects."""
import argparse
import pathlib
import shlex
import subprocess
import tempfile
import shutil


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('build', type=pathlib.Path)
    args = parser.parse_args()
    build = args.build.resolve()
    root = pathlib.Path(__file__).resolve().parent.parent
    temp = pathlib.Path(tempfile.mkdtemp(prefix='mongoose-origin-native-'))
    try:
        # Reuse the exact native server compilation flags, replacing only its
        # translation unit with the test that includes that source verbatim.
        if (build / 'build.ninja').exists():
            commands = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands'], text=True)
            command = next(line for line in commands.splitlines() if ' -c ' in line and '/src/server.cc' in line)
            compile_args = shlex.split(command)
        else:
            raise RuntimeError('Configure the native regression build with Ninja')
        compile_args[compile_args.index('-c') + 1] = str(root / 'test/test_network_binding_runtime.cc')
        compile_args[compile_args.index('-o') + 1] = str(temp / 'runtime.o')
        # Keep the configured server object's dependency file untouched.
        for flag, name in [('-MF', 'runtime.d'), ('-MT', 'runtime.o')]:
            if flag in compile_args:
                compile_args[compile_args.index(flag) + 1] = str(temp / name)
        objects = sorted((build / 'CMakeFiles/moo.dir').rglob('*.o'))
        objects = [str(path) for path in objects if path.name != 'server.cc.o']
        libraries = ['-lnettle', '-largon2', '-lpcre2-8', '-lcurl', '-laspell', '-lsqlite3', '-lssl', '-lcrypto', '-lm', '-lcrypt', '-pthread']
        sanitizer = [flag for flag in compile_args if flag.startswith('-fsanitize=')]
        if sanitizer:
            sanitizer.append('-no-pie')
        for extra in [[], ['-DTEST']]:
            subprocess.run([*compile_args, *extra], cwd=build, check=True)
            subprocess.run(['c++', *sanitizer, str(temp / 'runtime.o'), *objects, *libraries, '-o', str(temp / 'runtime')], check=True)
            subprocess.run([str(temp / 'runtime'), str(root / 'test/Test.db'), str(temp / 'unused.db')], cwd=temp, check=True)
        subprocess.run(['c++', '-std=c++17', '-I' + str(root / 'src/include'), '-I' + str(build),
                        str(root / 'test/test_network_origin.cc'), '-o', str(temp / 'counters')], check=True)
        subprocess.run([str(temp / 'counters')], check=True)
    except Exception:
        print(f'Native failure artifacts retained at {temp}', flush=True)
        raise
    else:
        shutil.rmtree(temp)


if __name__ == '__main__':
    main()
