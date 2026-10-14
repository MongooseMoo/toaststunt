require 'test_helper'

class TestRecycledWaifSlots < Test::Unit::TestCase
  def test_owned_waif_pins_recycled_slot_until_actual_free
    run_test_as('wizard') do
      owner, klass, holder = pinned_owner
      safe = create(:object)
      recycle(owner)
      recycle(safe)
      assert_equal 1, evaluate("waif_owner_count(#{owner})")
      assert_equal safe, evaluate("next_recycled_object(#{owner})")
      assert_equal E_INVARG, evaluate("recreate(#{owner}, $object)")
      assert_equal [0, 1], evaluate("{valid(#{owner}), waif_owner_count(#{owner})}")
      assert_equal safe, evaluate("recreate(#{safe}, $object)")
      assert_equal [1, 0, owner], simplify(command(%Q|; #{holder}.held = 0; pending = waif_owner_count(#{owner}); suspend(0); return {pending, waif_owner_count(#{owner}), next_recycled_object(#{owner})};|))
      assert_equal owner, evaluate("recreate(#{owner}, $object)")
      assert_equal 1, evaluate("valid(#{owner})")
    end
  end

  def test_class_waif_pins_recycled_slot_without_owner_collision
    run_test_as('wizard') do
      owner, klass, holder = pinned_owner
      safe = create(:object)
      recycle(klass)
      recycle(safe)
      assert_equal [0, 1], evaluate("{waif_owner_count(#{klass}), waif_stats()[#{klass}]}" )
      assert_equal safe, evaluate("next_recycled_object(#{klass})")
      assert_equal E_INVARG, evaluate("recreate(#{klass}, $object)")
      assert_equal [0, 1], evaluate("{valid(#{klass}), waif_owner_count(#{owner})}")
      assert_equal [0, klass], simplify(command(%Q|; #{holder}.held = 0; suspend(0); return {maphaskey(waif_stats(), #{klass}), next_recycled_object(#{klass})};|))
      assert_equal klass, evaluate("recreate(#{klass}, $object)")
    end
  end

  def test_pending_finalizer_and_resurrection_keep_slot_reserved
    run_test_as('wizard') do
      owner, klass, holder = pinned_owner
      add_property(holder, 'started', 0, [player, ''])
      add_verb(klass, [player, 'xd', ':recycle'], ['this', 'none', 'this'])
      set_verb_code(klass, ':recycle') do |code|
        code << "#{holder}.started = task_id();"
        code << 'suspend();'
        code << "#{holder}.held = this;"
      end
      safe = create(:object)
      recycle(owner)
      recycle(safe)
      assert_equal 1, simplify(command("; #{holder}.held = 0; return waif_owner_count(#{owner});"))
      task = get(holder, 'started')
      assert_operator task, :>, 0
      assert_equal safe, evaluate("next_recycled_object(#{owner})")
      assert_equal E_INVARG, evaluate("recreate(#{owner}, $object)")
      assert_equal 1, simplify(command("; resume(#{task}); suspend(0); return waif_owner_count(#{owner});"))
      assert_equal safe, evaluate("next_recycled_object(#{owner})")
      assert_equal E_INVARG, evaluate("recreate(#{owner}, $object)")
      assert_equal [1, 0, owner], simplify(command("; #{holder}.held = 0; before = waif_owner_count(#{owner}); suspend(0); return {before, waif_owner_count(#{owner}), next_recycled_object(#{owner})};"))
    end
  end

  def test_renumber_skips_reserved_destination_and_preserves_hierarchy
    run_test_as('wizard') do
      fill_existing_holes
      owner, klass, holder = pinned_owner
      safe = create(:object)
      mover = create(:object)
      child = create(mover)
      recycle(owner)
      recycle(safe)
      assert_equal safe, renumber(mover)
      assert_equal [0, 1, safe, 1], evaluate("{valid(#{owner}), valid(#{safe}), parent(#{child}), waif_owner_count(#{owner})}")
      simplify(command("; #{holder}.held = 0; suspend(0); return 0;"))
      assert_equal owner, renumber(safe)
      assert_equal owner, parent(child)
    end
  end

  def test_append_after_trimming_skips_reserved_slots
    run_test_as('wizard') do
      klass = create(:waif)
      holder = create(:object)
      add_property(holder, 'held', 0, [player, ''])
      owner = create(:object)
      owned_factory(klass, owner)
      simplify(command("; #{holder}.held = #{klass}:allocate(); return 0;"))
      recycle(owner)
      evaluate('reset_max_object()')
      fresh = create(:object)
      assert_not_equal owner, fresh
      assert_equal [0, 1, 1], evaluate("{valid(#{owner}), valid(#{fresh}), waif_owner_count(#{owner})}")
      simplify(command("; #{holder}.held = 0; suspend(0); return 0;"))
      assert_equal owner, evaluate("next_recycled_object(#{owner})")
    end
  end

  def test_recreate_keeps_error_precedence_and_initializer_continuation
    owner = klass = holder = other = nil
    run_test_as('wizard') do
      owner, klass, holder = pinned_owner
      other = player
      recycle(owner)
      assert_equal E_TYPE, evaluate('recreate(1, $object)')
      assert_equal E_INVARG, evaluate("recreate(#{owner}, toobj(2147483647))")
    end
    run_test_as('programmer') do
      assert_equal E_PERM, evaluate("recreate(#{owner}, $object, #{other})")
      assert_equal E_INVARG, evaluate("recreate(#{owner}, $object)")
    end
    run_test_as('wizard') do
      parent = create(:object)
      add_property(parent, 'calls', 0, [player, ''])
      add_verb(parent, [player, 'xd', 'initialize'], ['this', 'none', 'this'])
      set_verb_code(parent, 'initialize') do |code|
        code << "#{parent}.calls = #{parent}.calls + 1;"
        code << 'suspend(0);'
        code << 'add_property(this, "owned", 0, {this, ""});'
        code << "set_task_perms(this); this.owned = #{klass}:new();"
      end
      # The initializer's new owned WAIF is legitimate; return-stage must not
      # reapply the preallocation refusal to its own new lifetime.
      simplify(command("; #{holder}.held = 0; suspend(0); return 0;"))
      assert_equal owner, evaluate("recreate(#{owner}, #{parent})")
      assert_equal [1, 1, 1], evaluate("{valid(#{owner}), #{parent}.calls, waif_owner_count(#{owner})}")
      simplify(command("; #{owner}.owned = 0; suspend(0); return 0;"))
    end
  end

  private

  def pinned_owner
    owner = create(:object)
    klass = create(:waif)
    holder = create(:object)
    add_property(holder, 'held', 0, [player, ''])
    owned_factory(klass, owner)
    simplify(command("; #{holder}.held = #{klass}:allocate(); return 0;"))
    [owner, klass, holder]
  end

  def owned_factory(klass, owner)
    add_verb(klass, [owner, 'xd', 'allocate'], ['this', 'none', 'this'])
    set_verb_code(klass, 'allocate', ['return this:new();'])
  end

  def fill_existing_holes
    128.times do
      hole = evaluate('next_recycled_object()')
      return if hole == 0
      assert_equal hole, evaluate("recreate(#{hole}, $object)")
    end
    flunk 'isolated fixture contains too many preexisting holes'
  end
end
