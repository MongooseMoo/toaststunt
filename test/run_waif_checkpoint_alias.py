"""Compare invalid WAIF-class saving with and without a prior anonymous slot."""
import argparse
import pathlib
import socket
import tempfile

from run_map_order_regressions import run_server
from run_recycled_waif_regressions import ruby_case


def check_case(binary, prepare_binary, root, anonymous, recover_old):
    test_root = pathlib.Path(__file__).resolve().parent
    (root / "files").mkdir(parents=True)
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    (root / "test.yml").write_text(
        f"host: 127.0.0.1\nport: {port}\nverbose: false\n"
        "ownership_quota: false\n64bit: true\n"
    )
    anonymous_input = (
        'assert_equal [TYPE_ANON, 1], simplify(command("; '
        '#{holder}.anon_value = create(#{base}, 1); '
        'return {typeof(#{holder}.anon_value), valid(#{holder}.anon_value)};")); '
        if anonymous else ""
    )
    prepare = ruby_case((
        r'''assert_equal 0, evaluate('waif_stats()["total"]');
        base = create(:waif); add_property(base, ':payload', 0, [player, '']);
        leaf_class = create(:waif); leaf_owner = create(:object);
        add_verb(leaf_class, [leaf_owner, 'xd', 'allocate'], ['this', 'none', 'this']);
        set_verb_code(leaf_class, 'allocate', ['return this:new();']);
        holder = create(:object);
        assert_equal 0, add_property(holder, 'anon_value', 0, [player, '']);
        assert_equal 0, add_property(holder, 'waif_value', 0, [player, '']);
        assert_equal [2, 1, 1], evaluate("{length(properties(#{holder})), properties(#{holder})[1] == \"anon_value\", properties(#{holder})[2] == \"waif_value\"}");
        outer_class = create(base); '''
        + anonymous_input
        + r'''assert_equal [0, 1, 1, 2, 1, 1], simplify(command(";
        outer = #{outer_class}:new(); outer.payload = #{leaf_class}:allocate();
        #{holder}.waif_value = outer;
        add_property(#0, \"checkpoint_alias_fixture\", {#{holder}, #{outer_class}, #{leaf_owner}, player}, {player, \"\"});
        recycle(#{outer_class}); reset_max_object();
        return {valid(#{outer_class}), maphaskey(waif_stats(), #{outer_class}), waif_owner_count(#{leaf_owner}), waif_stats()[\"total\"], toint(max_object()) + 1 == toint(#{outer_class}), waif_owner_count(player)};"))'''
    ).replace("\n", " "))
    checkpoint = root / "prepared-checkpoint.db"
    result = run_server(
        prepare_binary, test_root / "Test.db", checkpoint, root, port, prepare
    )
    if result:
        return result
    # Counter observations precede every WAIF getter: a getter would lazily
    # invalidate the class and erase the save-order-dependent counterexample.
    report = r'''simplify(command(";
        f = #0.checkpoint_alias_fixture;
        return {maphaskey(waif_stats(), f[2]), waif_owner_count(f[3]), waif_stats()[\"total\"], waif_owner_count(f[4])};"))'''.replace("\n", " ")
    expected = "[1, 1, 2, 1]" if recover_old and anonymous else "[0, 0, 1, 1]"
    reloaded = root / "reloaded-checkpoint.db"
    result = run_server(
        binary, checkpoint, reloaded, root, port,
        ruby_case("assert_equal " + expected + ", " + report),
    )
    if result or not recover_old:
        return result
    # Loading an old damaged checkpoint alone does not repair its class.
    # Saving with the candidate normalizes it, preserving the outer owner.
    return run_server(
        binary, reloaded, root / "resaved-checkpoint.db", root, port,
        ruby_case("assert_equal [0, 0, 1, 1], " + report),
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=pathlib.Path)
    parser.add_argument("--prepare-with", type=pathlib.Path)
    parser.add_argument("--recover-old", action="store_true")
    parser.add_argument("--case", choices=("control", "anonymous", "both"), default="both")
    args = parser.parse_args()
    if args.recover_old and not args.prepare_with:
        parser.error("--recover-old requires --prepare-with an old writer")
    prepare_binary = args.prepare_with or args.binary
    cases = (False, True) if args.case == "both" else (args.case == "anonymous",)
    with tempfile.TemporaryDirectory(prefix="mongoose-waif-checkpoint-alias-") as temp:
        root = pathlib.Path(temp)
        for anonymous in cases:
            result = check_case(
                args.binary, prepare_binary,
                root / ("anonymous" if anonymous else "control"), anonymous,
                args.recover_old,
            )
            if result:
                return result
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
