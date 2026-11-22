require 'test_helper'

# recycle() runs the SAFE_RECYCLE owner fixup (a walk over every object, verb
# and property value) only for an object that an owner field names. These
# tests check that every owner field still naming the recycled object becomes
# #-1, exactly as before, through every way an owner field is written or
# removed, and that an object that gave away everything it owned is recycled
# without the walk.
class TestOwnerCount < Test::Unit::TestCase
  def moo(code)
    simplify(command("; #{code}"))
  end

  def test_fixup_clears_every_owner_field_naming_the_recycled_object
    run_test_as('wizard') do
      result = moo(<<~MOO.tr("\n", ' '))
        a = create(#-1); a.owner = a;
        o = create(#-1, a);
        add_property(o, "held_p", 1, {a, "r"});
        add_verb(o, {a, "rx", "v"}, {"this", "none", "this"});
        parent = create(#-1);
        add_property(parent, "held_q", 0, {a, "r"});
        add_property(parent, "held_c", 0, {a, "rc"});
        child = create(parent, player);
        kept = create(#-1, player);
        add_property(kept, "held_k", 0, {player, "r"});
        recycle(a);
        return {o.owner == #-1, property_info(o, "held_p")[1] == #-1, verb_info(o, "v")[1] == #-1,
                property_info(parent, "held_q")[1] == #-1, property_info(child, "held_q")[1] == #-1,
                property_info(child, "held_c")[1] == player, kept.owner == player, property_info(kept, "held_k")[1] == player};
      MOO
      assert_equal [1, 1, 1, 1, 1, 1, 1, 1], result
    end
  end

  def test_ownership_moved_by_every_writer_is_still_fixed_up
    run_test_as('wizard') do
      result = moo(<<~MOO.tr("\n", ' '))
        a = create(#-1); b = create(#-1);
        o = create(#-1, a);
        add_property(o, "held_p", 1, {a, "r"});
        add_verb(o, {a, "rx", "v"}, {"this", "none", "this"});
        o.owner = b;
        set_property_info(o, "held_p", {b, "r"});
        set_verb_info(o, "v", {b, "rx", "v"});
        recycle(a);
        before = {o.owner == b, property_info(o, "held_p")[1] == b, verb_info(o, "v")[1] == b};
        recycle(b);
        return {@before, o.owner == #-1, property_info(o, "held_p")[1] == #-1, verb_info(o, "v")[1] == #-1};
      MOO
      assert_equal [1, 1, 1, 1, 1, 1], result
    end
  end

  def test_chparent_and_renumber_keep_the_count
    run_test_as('wizard') do
      result = moo(<<~MOO.tr("\n", ' '))
        free = create(#-1);
        a = create(#-1);
        parent = create(#-1);
        add_property(parent, "held_q", 0, {a, "r"});
        child = create(#-1, player);
        chparent(child, parent);
        moved = property_info(child, "held_q")[1] == a;
        holder = create(#-1, a);
        loose = create(parent, player);
        chparent(loose, #-1);
        recycle(free);
        b = renumber(a);
        renamed = {b != a && b < a, holder.owner == b, property_info(child, "held_q")[1] == b};
        recycle(b);
        return {moved, @renamed, holder.owner == #-1, property_info(child, "held_q")[1] == #-1, property_info(parent, "held_q")[1] == #-1};
      MOO
      assert_equal [1, 1, 1, 1, 1, 1, 1], result
    end
  end

  def test_anonymous_holders_are_still_fixed_up
    run_test_as('wizard') do
      result = moo(<<~MOO.tr("\n", ' '))
        a = create(#-1);
        parent = create(#-1);
        add_property(parent, "held_q", 0, {a, "r"});
        x = create(parent, a, 1);
        y = create(parent, player, 1);
        recycle(a);
        return {x.owner == #-1, property_info(y, "held_q")[1] == #-1};
      MOO
      assert_equal [1, 1], result
    end
  end

  # An owner that gave away everything recycles as fast as one that never
  # owned anything; one that still owns something pays for the walk. The
  # database is grown first so that the walk takes measurable time.
  def test_an_object_that_gave_everything_away_is_recycled_without_the_walk
    run_test_as('wizard') do
      moo('add_property(#0, "owner_count_fill", {}, {player, ""}); return 1;')
      20.times do
        assert_equal 1, moo(<<~MOO.tr("\n", ' '))
          made = {};
          for i in [1..500]
            x = create(#-1);
            for p in ({"fill_a", "fill_b", "fill_c", "fill_d", "fill_e", "fill_f", "fill_g", "fill_h"})
              add_property(x, p, 0, {player, "r"});
            endfor
            made = {@made, x};
          endfor
          f = #0.owner_count_fill; #0.owner_count_fill = {@f, @made};
          return 1;
        MOO
      end
      timings = moo(<<~MOO.tr("\n", ' '))
        owner = {}; gave = {}; never = {};
        for i in [1..7]
          a = create(#-1); o = create(#-1, a);
          t = ftime(); recycle(a); owner = {@owner, ftime() - t};
          recycle(o);
          a = create(#-1); o = create(#-1, a);
          add_property(o, "held_p", 0, {a, "r"});
          add_verb(o, {a, "rx", "v"}, {"this", "none", "this"});
          o.owner = player; set_property_info(o, "held_p", {player, "r"}); set_verb_info(o, "v", {player, "rx", "v"});
          t = ftime(); recycle(a); gave = {@gave, ftime() - t};
          recycle(o);
          a = create(#-1);
          t = ftime(); recycle(a); never = {@never, ftime() - t};
        endfor
        return {sort(owner)[4], sort(gave)[4], sort(never)[4]};
      MOO
      moo('for x in (#0.owner_count_fill) recycle(x); endfor delete_property(#0, "owner_count_fill"); return 1;')
      owner, gave, never = timings
      message = "median recycle: still an owner #{owner}s, gave everything away #{gave}s, never owned anything #{never}s"
      assert owner > 4 * never, "the walk is not measurable; " + message
      assert gave < (owner + never) / 2, message
    end
  end
end
