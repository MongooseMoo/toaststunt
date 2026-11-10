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
    add_verb(object, [player, 'rxd', name], ['this', 'none', 'this'])
    assert_equal [], set_verb_code(object, name, source)
  end

  def record(object, expression)
    simplify(command(%Q|; coverage_start(); #{expression}; return coverage_stop();|))
  end

  def test_exact_branch_lines_and_aliases_use_the_defining_verb
    with_fixture do |o|
      define(o, 'branch alias', ['if (args[1])', 'return 11;', 'else', 'return 22;', 'endif'])
      assert_equal({"#{o}:branch alias" => [1, 2]}, record(o, "#{o}:alias(1)"))
      assert_equal({"#{o}:branch alias" => [1, 4]}, record(o, "#{o}:branch(0)"))
      child = create(o)
      begin
        assert_equal({"#{o}:branch alias" => [1, 2]}, record(o, "#{child}:branch(1)"))
      ensure
        recycle(child)
      end
    end
  end

  def test_hits_are_sorted_unique_and_accumulate_across_calls
    with_fixture do |o|
      define(o, 'branch', ['if (args[1])', 'return 11;', 'else', 'return 22;', 'endif'])
      assert_equal({"#{o}:branch" => [1, 2, 4]}, record(o, "#{o}:branch(0); #{o}:branch(1); #{o}:branch(0)"))
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

  def test_fork_vectors_and_grandchildren_are_included
    with_fixture do |o|
      define(o, 'leaf', ['return 123;'])
      define(o, 'spawn', [
        'fork (0)', 'fork (0)', 'this:leaf();', 'endfork', 'endfork', 'return 1;'
      ])
      # Let all ready child work finish; no wall-clock delay.
      result = record(o, "#{o}:spawn(); suspend(0); suspend(0)")
      assert_equal [1], result["#{o}:leaf"]
      assert_include result["#{o}:spawn"], 3
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
      result = simplify(command(%Q|; fork id (0) #{o}:waiter(); endfork suspend(0); coverage_start(); resume(id); suspend(0); return coverage_stop();|))
      assert_equal [1], result["#{o}:leaf"]
      assert_equal [2, 3], result["#{o}:waiter"]
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
      result = record(o, %Q|#{o}:leaf(); set_verb_code(#{o}, "leaf", {"return 456;"}); #{o}:leaf()|)
      assert_equal({"!changed:#{o}:leaf" => [], "#{o}:leaf" => [1]}, result)
    end
  end

  def test_recompiling_a_suspended_activation_does_not_report_stale_lines
    with_fixture do |o|
      define(o, 'waiter', ['suspend();', 'return 123;'])
      result = simplify(command(%Q|; fork id (0) #{o}:waiter(); endfork suspend(0); coverage_start(); set_verb_code(#{o}, "waiter", {"return 456;"}); resume(id); suspend(0); return coverage_stop();|))
      assert_equal({"!changed:#{o}:waiter" => []}, result)
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
end
