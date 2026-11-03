require 'test_helper'

class TestWaifOwnerCount < Test::Unit::TestCase
  def test_counts_instances_not_aliases_and_keeps_pending_finalizers
    run_test_as('wizard') do
      owner = create(:object)
      a = create(:waif)
      b = create(:waif)
      result = simplify(command(%Q|; set_task_perms(#{owner}); x = #{a}:new(); y = #{b}:new(); refs = {x, x, y}; before = waif_owner_count(#{owner}); x = y = refs = 0; pending = waif_owner_count(#{owner}); suspend(0); return {before, pending, waif_owner_count(#{owner}), valid(#{a}), valid(#{b})};|))
      assert_equal [2, 2, 0, 1, 1], result
    end
  end

  def test_recycle_observes_allocation_and_resurrection_stays_counted
    run_test_as('wizard') do
      owner = create(:object)
      klass = create(:waif)
      add_property(klass, 'held', 0, [owner, 'r'])
      add_property(klass, 'seen', [], [owner, 'r'])
      add_verb(klass, [owner, 'xd', ':recycle'], ['this', 'none', 'this'])
      set_verb_code(klass, ':recycle') do |vc|
        vc << %Q|#{klass}.seen = {@#{klass}.seen, waif_owner_count(this.owner)};|
        vc << %Q|#{klass}.held = this;|
      end
      assert_equal 1, simplify(command(%Q|; set_task_perms(#{owner}); w = #{klass}:new(); w = 0; return waif_owner_count(#{owner});|))
      assert_equal [1, 1], evaluate("{length(#{klass}.seen), #{klass}.seen[1]}")
      assert_equal 1, evaluate("waif_owner_count(#{owner})")
      assert_equal [1, 0], simplify(command(%Q|; #{klass}.held = 0; before = waif_owner_count(#{owner}); suspend(0); return {before, waif_owner_count(#{owner})};|))
      assert_equal [1, 1], evaluate("{length(#{klass}.seen), #{klass}.seen[1]}")
    end
  end

  def test_owner_isolation_and_wizard_or_self_authority
    first = second = nil
    run_test_as('wizard') do
      first = create(:object)
      second = create(:object)
      klass = create(:waif)
      add_property(klass, 'held', [], [player, ''])
      owned_factory(klass, first, 'first')
      owned_factory(klass, second, 'second')
      assert_equal [1, 2], simplify(command(%Q|; a = #{klass}:first(); b = #{klass}:second(); c = #{klass}:second(); #{klass}.held = {a, b, c}; return {waif_owner_count(#{first}), waif_owner_count(#{second})};|))
      assert_equal 1, simplify(command(%Q|; set_task_perms(#{first}); return waif_owner_count(#{first});|))
      assert_equal E_PERM, simplify(command(%Q|; set_task_perms(#{first}); return waif_owner_count(#{second});|))
      assert_equal 0, evaluate('waif_owner_count(#-1)')
      assert_equal 0, evaluate('waif_owner_count(toobj(2147483647))')
      set(klass, 'held', 0)
    end
    run_test_as('programmer') do
      assert_kind_of Integer, evaluate('waif_owner_count(player)')
      assert_equal E_PERM, evaluate("waif_owner_count(#{first})")
      assert_equal E_PERM, evaluate('waif_owner_count(#-1)')
    end
  end

  def test_invalidated_class_and_recycled_owner_do_not_erase_allocations
    run_test_as('wizard') do
      owner = create(:object)
      klass = create(:waif)
      holder = create(:object)
      add_property(holder, 'held', 0, [player, ''])
      owned_factory(klass, owner, 'allocate')
      assert_equal 1, simplify(command(%Q|; w = #{klass}:allocate(); #{holder}.held = w; return waif_owner_count(#{owner});|))
      recycle(klass)
      assert_equal [NOTHING, 1], simplify(command(%Q|; return {#{holder}.held.class, waif_owner_count(#{owner})};|))
      recycle(owner)
      assert_equal 1, evaluate("waif_owner_count(#{owner})")
      assert_equal [1, 0], simplify(command(%Q|; #{holder}.held = 0; before = waif_owner_count(#{owner}); suspend(0); return {before, waif_owner_count(#{owner})};|))
    end
  end

  def test_native_argument_errors_and_registry_capacity
    run_test_as('wizard') do
      assert_equal E_ARGS, evaluate('call_function("waif_owner_count")')
      assert_equal E_ARGS, evaluate('call_function("waif_owner_count", player, player)')
      [0, '"owner"', '{}', '[]', '$waif:new()', 'create({}, 1)'].each do |value|
        assert_equal E_TYPE, evaluate("waif_owner_count(#{value})")
      end
      assert_equal ['waif_owner_count', 1, 1, 1, TYPE_OBJ], evaluate('{function_info("waif_owner_count")[1], function_info("waif_owner_count")[2], function_info("waif_owner_count")[3], length(function_info("waif_owner_count")[4]), function_info("waif_owner_count")[4][1]}')
      assert_operator evaluate('length(function_info())'), :<, 256
    end
  end

  def test_normal_builtin_protection_and_own_hook
    run_test_as('wizard') do
      evaluate('add_property($server_options, "protect_waif_owner_count", 1, {player, ""})')
      evaluate('load_server_options()')
    end
    begin
      run_test_as('programmer') do
        assert_equal E_PERM, evaluate('waif_owner_count(player)')
        assert_equal E_PERM, evaluate('waif_owner_count(0)')
      end
      run_test_as('wizard') do
        assert_kind_of Integer, evaluate('waif_owner_count(player)')
        add_verb(SYSTEM, [player, 'xd', 'bf_waif_owner_count'], ['this', 'none', 'this'])
        set_verb_code(SYSTEM, 'bf_waif_owner_count') { |vc| vc << 'return {"owner-count-hook", args[1], length(args)};' }
      end
      run_test_as('programmer') do
        assert_equal ['owner-count-hook', player, 1], evaluate('waif_owner_count(player)')
      end
    ensure
      run_test_as('wizard') do
        delete_verb(SYSTEM, 'bf_waif_owner_count') if evaluate('is_member("bf_waif_owner_count", verbs(#0))') != 0
        evaluate('delete_property($server_options, "protect_waif_owner_count")')
        evaluate('load_server_options()')
      end
    end
  end

  private

  def owned_factory(klass, owner, name)
    add_verb(klass, [owner, 'xd', name], ['this', 'none', 'this'])
    set_verb_code(klass, name) { |vc| vc << 'return this:new();' }
  end
end
