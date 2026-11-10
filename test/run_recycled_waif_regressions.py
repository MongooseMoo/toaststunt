"""Check recycled-slot guards across real old and new native checkpoints."""
import argparse
import pathlib
import socket
import tempfile

from run_map_order_regressions import run_server


def ruby_case(body):
    return [
        "-e",
        "require 'test_helper'; class TestRecycledSlotReload < Test::Unit::TestCase; "
        "def test_reload; run_test_as('wizard') do; " + body + "; end; end; end",
    ]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=pathlib.Path)
    parser.add_argument("--prepare-with", type=pathlib.Path, required=True)
    args = parser.parse_args()
    test_root = pathlib.Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="mongoose-recycled-waif-") as temp:
        root = pathlib.Path(temp)
        (root / "files").mkdir()
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        (root / "test.yml").write_text(
            f"host: 127.0.0.1\nport: {port}\nverbose: false\n"
            "ownership_quota: false\n64bit: true\n"
        )
        prepare = ruby_case(
            'klass = create(:waif); owner = create(:object); '
            'add_verb(klass, [owner, "xd", "allocate"], ["this", "none", "this"]); '
            'set_verb_code(klass, "allocate", ["return this:new();"]); '
            'assert_equal [0, 1], simplify(command("; '
            'add_property(#0, \\\"recycled_slot_fixture\\\", {#{owner}, #{klass}:allocate()}, {player, \\\"\\\"}); '
            'recycle(#{owner}); reset_max_object(); '
            'return {valid(#{owner}), waif_owner_count(#{owner})};"))'
        )
        checkpoint = root / "old-checkpoint.db"
        result = run_server(
            args.prepare_with, test_root / "Test.db", checkpoint, root, port, prepare
        )
        if result:
            return result
        verify = (
            'owner = simplify(command("; return #0.recycled_slot_fixture[1];")); '
            'assert_equal [0, 1, 1], simplify(command("; '
            'o = #0.recycled_slot_fixture[1]; '
            'return {valid(o), waif_owner_count(o), next_recycled_object(o) != o};")); '
            'assert_equal E_INVARG, evaluate("recreate(#{owner}, $object)"); '
            'fresh = create(:object); assert_not_equal owner, fresh; '
            'assert_equal [0, 1], evaluate("{valid(#{owner}), waif_owner_count(#{owner})}")'
        )
        fixed_checkpoint = root / "fixed-checkpoint.db"
        result = run_server(
            args.binary, checkpoint, fixed_checkpoint, root, port, ruby_case(verify)
        )
        if result:
            return result
        release = (
            verify + '; assert_equal [1, 0, owner], simplify(command("; '
            'o = #0.recycled_slot_fixture[1]; #0.recycled_slot_fixture = {}; '
            'before = waif_owner_count(o); suspend(0); '
            'return {before, waif_owner_count(o), next_recycled_object(o)};")); '
            'assert_equal owner, evaluate("recreate(#{owner}, $object)")'
        )
        return run_server(
            args.binary, fixed_checkpoint, root / "second-checkpoint.db", root,
            port, ruby_case(release),
        )


if __name__ == "__main__":
    raise SystemExit(main())
