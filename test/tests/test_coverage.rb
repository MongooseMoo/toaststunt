require 'test_helper'

class TestCoverage < Test::Unit::TestCase
  def with_fixture
    run_test_as('wizard') do
      object = create(NOTHING)
      begin
        yield object
      ensure
        recycle(object)
      end
    end
  end

  def define(object, name, source)
    index = add_verb(object, [player, 'rxd', name], ['this', 'none', 'this'])
    assert_kind_of Integer, index
    assert_operator index, :>, 0
    assert_equal [], set_verb_code(object, index, source)
  end

  def assert_coverage(expected, statements)
    actual = simplify(command(%Q^; #{statements}; c = coverage_stop(); return equal(c, #{value_ref(expected)}) || toliteral(c);^))
    assert_equal 1, actual
  end

  def assert_record(expected, expression)
    assert_coverage(expected, "coverage_start(); #{expression}")
  end

  def test_exact_branch_lines_and_aliases_use_the_defining_verb
    with_fixture do |o|
      define(o, 'branch alias', ['if (args[1])', 'return 11;', 'else', 'return 22;', 'endif'])
      assert_record({"#{o}:branch alias" => [1, 2]}, "#{o}:alias(1)")
      assert_record({"#{o}:branch alias" => [1, 4]}, "#{o}:branch(0)")
      child = create(o)
      begin
        assert_record({"#{o}:branch alias" => [1, 2]}, "#{child}:branch(1)")
      ensure
        recycle(child)
      end
    end
  end

  def test_hits_are_sorted_unique_and_accumulate_across_calls
    with_fixture do |o|
      define(o, 'branch', ['if (args[1])', 'return 11;', 'else', 'return 22;', 'endif'])
      assert_record({"#{o}:branch" => [1, 2, 4]}, "#{o}:branch(0); #{o}:branch(1); #{o}:branch(0)")
    end
  end

  def test_permission_arity_and_session_errors
    run_test_as('programmer') do
      assert_equal E_PERM, evaluate('coverage_start()')
      assert_equal E_PERM, evaluate('coverage_stop()')
    end
    run_test_as('wizard') do
      assert_equal E_ARGS, evaluate('coverage_start(1)')
      assert_equal E_ARGS, evaluate('coverage_stop(1)')
      assert_equal E_INVARG, evaluate('coverage_stop()')
      assert_equal [E_INVARG, {}], simplify(command('; coverage_start(); e = `coverage_start() ! ANY\'; c = coverage_stop(); return {e, c};'))
      assert_equal [{}, {}], simplify(command('; coverage_start(); a = coverage_stop(); coverage_start(); return {a, coverage_stop()};'))
      assert_equal 0, evaluate('coverage_start()')
      assert_equal E_INVARG, evaluate('coverage_stop()') # the previous task finished
    end
  end

  def test_existing_builtins_at_the_end_of_registration_are_preserved
    run_test_as('wizard') do
      assert_equal 'validate_ast', evaluate('function_info("validate_ast")[1]')
    end
  end

  def test_fork_vectors_and_grandchildren_are_included
    with_fixture do |o|
      define(o, 'leaf', ['return 123;'])
      define(o, 'spawn', [
        'fork (0)', 'fork (0)', 'this:leaf();', 'endfork', 'endfork', 'return 1;'
      ])
      # Let all ready child work finish; no wall-clock delay.
      assert_record({"#{o}:leaf" => [1], "#{o}:spawn" => [1, 2, 3, 6]},
        "#{o}:spawn(); suspend(0); suspend(0)")
    end
  end

  def test_stop_detaches_a_pending_child
    with_fixture do |o|
      define(o, 'probe', ['return `coverage_stop() ! ANY\';'])
      define(o, 'spawn', [
        'fork (0)', 'this.result = this:probe();', 'endfork', 'return 1;'
      ])
      add_property(o, 'result', 0, [player, 'rw'])
      result = simplify(command(%Q|; coverage_start(); #{o}:spawn(); c = coverage_stop(); suspend(0); return #{o}.result;|))
      assert_equal E_INVARG, result
    end
  end

  def test_explicit_resume_adopts_an_existing_task
    with_fixture do |o|
      define(o, 'leaf', ['return 123;'])
      define(o, 'waiter', ['suspend();', 'this:leaf();', 'return 0;'])
      # Create and suspend the child before coverage starts.
      # The continuation after suspend() still executes opcodes on line 1.
      assert_coverage({"#{o}:leaf" => [1], "#{o}:waiter" => [1, 2, 3]},
        "fork id (0) #{o}:waiter(); endfork suspend(0); coverage_start(); resume(id); suspend(0)")
    end
  end

  def test_unrelated_tasks_are_not_recorded
    with_fixture do |o|
      define(o, 'leaf', ['return 123;'])
      result = simplify(command(%Q|; fork (0) #{o}:leaf(); endfork coverage_start(); suspend(0); return coverage_stop();|))
      assert_equal({}, result)
    end
  end

  def test_recompilation_discards_old_lines_and_reports_the_change
    with_fixture do |o|
      define(o, 'leaf', ['x = 123;', 'return x;'])
      assert_record({"!changed:#{o}:leaf" => [], "#{o}:leaf" => [1]},
        %Q|#{o}:leaf(); set_verb_code(#{o}, "leaf", {"return 456;"}); #{o}:leaf()|)
    end
  end

  def test_recompiling_a_suspended_activation_does_not_report_stale_lines
    with_fixture do |o|
      define(o, 'waiter', ['suspend();', 'return 123;'])
      assert_coverage({"!changed:#{o}:waiter" => []},
        %Q|fork id (0) #{o}:waiter(); endfork suspend(0); coverage_start(); set_verb_code(#{o}, "waiter", {"return 456;"}); resume(id); suspend(0)|)
    end
  end

  def test_tick_cost_is_identical_with_recording_off_and_on
    with_fixture do |o|
      define(o, 'measure', ['before = ticks_left();', 'x = 1 + 2;', 'return before - ticks_left();'])
      result = simplify(command(%Q|; off = #{o}:measure(); coverage_start(); on = #{o}:measure(); coverage_stop(); after = #{o}:measure(); return {off, on, after};|))
      assert_kind_of Array, result
      assert_operator result[0], :>, 0
      assert_equal [result[0]] * 3, result
    end
  end

  def test_implicit_return_does_not_report_a_line_past_the_source
    with_fixture do |o|
      define(o, 'implicit', ['x = 123;'])
      assert_record({"#{o}:implicit" => [1]}, "#{o}:implicit()")
    end
  end

  def test_owner_completion_discards_coverage_before_a_child_runs
    with_fixture do |o|
      add_property(o, 'result', 999, [player, 'rw'])
      define(o, 'spawn', [
        'coverage_start();',
        'fork (0)',
        'this.result = `coverage_start() ! ANY\';',
        'coverage_stop();',
        'endfork',
        'return 1;'
      ])
      # Both Ruby commands are separate MOO tasks; the owner finishes first.
      assert_equal 1, evaluate("#{o}:spawn()")
      assert_equal 0, simplify(command(%Q|; suspend(0); return #{o}.result;|))
    end
  end

  def test_killing_the_suspended_owner_detaches_its_children
    with_fixture do |o|
      add_property(o, 'child', 0, [player, 'rw'])
      add_property(o, 'result', 999, [player, 'rw'])
      define(o, 'owner', [
        'coverage_start();',
        'fork child (0)',
        'suspend();',
        'this.result = `coverage_start() ! ANY\';',
        'coverage_stop();',
        'endfork',
        'this.child = child;',
        'suspend();'
      ])
      result = simplify(command(%Q|; fork owner (0) #{o}:owner(); endfork suspend(0); suspend(0); kill_task(owner); resume(#{o}.child); suspend(0); return #{o}.result;|))
      assert_equal 0, result
    end
  end

  def test_independent_recordings_cannot_be_combined_by_resume
    with_fixture do |o|
      define(o, 'owner', ['coverage_start();', 'suspend();', 'coverage_stop();'])
      result = simplify(command(%Q|; fork owner (0) #{o}:owner(); endfork suspend(0); coverage_start(); e = `resume(owner) ! ANY'; c = coverage_stop(); kill_task(owner); return {e, c};|))
      assert_equal [E_INVARG, {}], result
    end
  end

  # The statements leave their value in r; it is compared natively.
  def assert_result(expected, statements)
    actual = simplify(command(%Q^; #{statements}; return equal(r, #{value_ref(expected)}) || toliteral(r);^))
    assert_equal 1, actual
  end

  # Server panic, 2026-11-14 05:03 UTC: "Missing END_CATCH in DECOMPILE!".
  def test_catch_expressions_are_mapped
    with_fixture do |o|
      define(o, 'catcher', ['x = `args[1] ! ANY\';', 'y = `args[9] ! E_RANGE => 7\';', 'return x + y;'])
      assert_record({"#{o}:catcher" => [1, 2, 3]}, "#{o}:catcher(1)")
    end
  end

  # Server panic, 2026-11-14 05:29 UTC: "Can't do job in FIND_LINE_NUMBER!".
  def test_indexed_and_ranged_assignments_are_mapped
    with_fixture do |o|
      add_property(o, 'p', 0, [player, 'rw'])
      define(o, 'assign', [
        'x = {1, 2, 3};', 'x[2] = 5;', 'x[1..2] = {8, 9};', 'y = {x};', 'y[1][2] = 4;',
        'this.p = x;', 'this.p[1] = x[$];', 'm = ["a" -> 1, "b" -> x[1]];', 'm["c"] = y;', 'return m;'
      ])
      assert_record({"#{o}:assign" => [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]}, "#{o}:assign()")
    end
  end

  def test_every_statement_kind_is_mapped
    with_fixture do |o|
      add_property(o, 'p', 0, [player, 'rw'])
      define(o, 'kinds', [
        'x = {1, 5, 3};',          # 1
        'y = 0;',                  # 2
        'for v in (x)',            # 3
        'if (v == 5)',             # 4
        'continue;',               # 5
        'elseif (v == 3)',         # 6
        'break;',                  # 7
        'else',                    # 8
        'y = y + v;',              # 9
        'endif',                   # 10
        'endfor',                  # 11
        'for i, k in (["a" -> 1])',  # 12
        'y = y + i;',              # 13
        'endfor',                  # 14
        'for i in [1..2]',         # 15
        'y = y + i;',              # 16
        'endfor',                  # 17
        'while (y > 100)',         # 18
        'y = y - 1;',              # 19
        'endwhile',                # 20
        'while w (1)',             # 21
        'break w;',                # 22
        'endwhile',                # 23
        'try',                     # 24
        'x[10] = 1;',              # 25
        'y = 1000;',               # 26
        'except e (E_RANGE)',      # 27
        'y = y + 1;',              # 28
        'endtry',                  # 29
        'try',                     # 30
        'y = y + 1;',              # 31
        'finally',                 # 32
        'y = y + 1;',              # 33
        'endtry',                  # 34
        '{a, ?b = 2, @c} = {1};',  # 35
        'y = (a && b) || (c ? 1 | 2);',  # 36
        'y = ~y ^ 1 |. 2 &. 3 << 1 >> 1;',  # 37
        'fork (0)',                # 38
        'this.p = 1;',             # 39
        'endfork',                 # 40
        'suspend(0);',             # 41
        'return y;'                # 42
      ])
      # Never reached: 19 (false loop), 26 (after the raise). `else' and `endif'
      # have no instructions. A loop's closing line is its backward jump.
      assert_record({"#{o}:kinds" => [1, 2, 3, 4, 5, 6, 7, 9, 11, 12, 13, 14, 15, 16, 17, 18,
                                      21, 22, 24, 25, 27, 28, 30, 31, 32, 33, 34, 35, 36, 37,
                                      38, 39, 41, 42]}, "#{o}:kinds()")
    end
  end

  # The sequence that panicked the live server: start, stop, start, a verb
  # call, stop, start, stop, in one task across verb frames.
  def test_stop_and_start_inside_a_verb_called_while_recording
    with_fixture do |o|
      define(o, 'leaf', ['return 123;'])
      define(o, 'inner', [
        'a = coverage_stop();', 'coverage_start();', 'x = `this:leaf() ! ANY\';',
        'b = coverage_stop();', 'coverage_start();', 'return {a, b};'
      ])
      assert_result([{"#{o}:inner" => [1]},
                     {"#{o}:inner" => [2, 3, 4], "#{o}:leaf" => [1]},
                     {"#{o}:inner" => [5, 6]}],
        "coverage_start(); r = #{o}:inner(); r = {@r, coverage_stop()}")
    end
  end

  def test_thousands_of_sequential_recordings_in_one_task
    with_fixture do |o|
      define(o, 'leaf', ['x = {1};', 'x[1] = `x[2] ! ANY => 2\';', 'return x;'])
      assert_result([2000, {"#{o}:leaf" => [1, 2, 3]}],
        "n = 0; for i in [1..2000] coverage_start(); #{o}:leaf(); c = coverage_stop(); n = n + (length(c) == 1); endfor r = {n, c}")
    end
  end

  def test_second_start_inside_a_called_verb_raises_and_keeps_recording
    with_fixture do |o|
      define(o, 'again', ['return `coverage_start() ! ANY\';'])
      assert_result([E_INVARG, {"#{o}:again" => [1]}],
        "coverage_start(); e = #{o}:again(); r = {e, coverage_stop()}")
    end
  end

  def test_a_child_stopping_the_recording_leaves_the_owner_idle
    with_fixture do |o|
      define(o, 'leaf', ['return 123;'])
      assert_result([E_INVARG, 0],
        "coverage_start(); fork (0) coverage_stop(); endfork suspend(0); #{o}:leaf(); e = `coverage_stop() ! ANY'; r = {e, coverage_start()}; coverage_stop()")
    end
  end

  def test_recording_continues_past_suspend_and_killed_children
    with_fixture do |o|
      define(o, 'leaf', ['return 123;'])
      define(o, 'sleeper', ['suspend();', 'this:leaf();'])
      assert_result({"#{o}:leaf" => [1], "#{o}:sleeper" => [1]},
        "coverage_start(); fork t (0) #{o}:sleeper(); endfork fork u (100) #{o}:sleeper(); endfork suspend(0); kill_task(t); kill_task(u); suspend(0); #{o}:leaf(); r = coverage_stop()")
    end
  end

  def test_children_outliving_the_stop_run_and_record_again
    with_fixture do |o|
      add_property(o, 'result', 0, [player, 'rw'])
      define(o, 'leaf', ['return 123;'])
      define(o, 'child', [
        'suspend();', 'this:leaf();', 'coverage_start();', 'this:leaf();', 'this.result = coverage_stop();'
      ])
      # One child is suspended inside a recorded verb and one has not yet run
      # when the recording stops.
      assert_result([{"#{o}:child" => [1]}, {"#{o}:child" => [3, 4, 5], "#{o}:leaf" => [1]}],
        "coverage_start(); fork t (0) #{o}:child(); endfork suspend(0); fork (0) #{o}:leaf(); endfork c = coverage_stop(); resume(t); suspend(0); r = {c, #{o}.result}")
    end
  end

  # Tracebacks use the same lookup: an error in a later map pair, and a
  # refused property store after an indexed assignment.
  def test_traceback_lines_for_map_pairs_and_indexed_property_stores
    with_fixture do |o|
      add_property(o, 'p', 0, [player, 'r'])
      define(o, 'pairs', ['x = 0;', 'return ["a" -> 1, "b" -> 1 / x];', 'x = 1;'])
      define(o, 'store', ['x = 0;', 'this.p = {1};', 'set_task_perms(this);', 'this.p[1] = 2;', 'x = 1;'])
      define(o, 'line', ['try', 'this:(args[1])();', 'except e (ANY)', 'return {e[1], e[4][1][6]};', 'endtry'])
      assert_equal [E_DIV, 2], evaluate("#{o}:line(\"pairs\")")
      assert_equal [E_PERM, 4], evaluate("#{o}:line(\"store\")")
    end
  end

  def test_recompiling_the_running_verb_then_stopping_inside_it
    with_fixture do |o|
      define(o, 'self', ['set_verb_code(this, "self", {"return 1;"});', 'x = 1;', 'return coverage_stop();'])
      assert_result({"!changed:#{o}:self" => []}, "coverage_start(); r = #{o}:self()")
    end
  end
end
