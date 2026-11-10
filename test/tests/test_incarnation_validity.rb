require 'test_helper'

class TestIncarnationValidity < Test::Unit::TestCase
  def test_bad_argument_types_keep_native_errors
    run_test_as('wizard') do
      assert_equal E_TYPE, evaluate('valid(42)')
      assert_equal E_TYPE, evaluate('valid(player, 42)')
      @sock.close
    end
  end

  def test_inherited_identity_on_anonymous_objects
    run_test_as('wizard') do
      parent = create(NOTHING)
      add_property(parent, 'incarnation_id', 'inherited-epoch', %Q|{#{obj_ref(player)}, "r"}|)
      result = simplify(command(%Q|; a = create(#{obj_ref(parent)}, player, 1); return {valid(a), valid(a, "inherited-epoch"), valid(a, "other-epoch")};|))
      assert_equal [1, 1, 0], result
      recycle(parent)
      @sock.close
    end
  end

  def test_public_identity_is_exact_and_lifetime_pinned
    run_test_as('wizard') do
      actor = create(NOTHING)
      add_property(actor, 'incarnation_id', 'epoch-a', %Q|{#{obj_ref(player)}, "r"}|)
      assert_equal 1, evaluate("valid(#{obj_ref(actor)})")
      assert_equal 1, evaluate(%Q|valid(#{obj_ref(actor)}, "epoch-a")|)
      assert_equal 0, evaluate(%Q|valid(#{obj_ref(actor)}, "epoch-A")|)
      set(actor, 'incarnation_id', 'epoch-b')
      assert_equal 0, evaluate(%Q|valid(#{obj_ref(actor)}, "epoch-a")|)
      assert_equal 1, evaluate(%Q|valid(#{obj_ref(actor)}, "epoch-b")|)
      recycle(actor)
      assert_equal 0, evaluate(%Q|valid(#{obj_ref(actor)}, "epoch-b")|)
      @sock.close
    end
  end

  def test_missing_empty_and_nonstring_identities_do_not_match
    run_test_as('wizard') do
      actor = create(NOTHING)
      assert_equal 1, evaluate("valid(#{obj_ref(actor)})")
      assert_equal 0, evaluate(%Q|valid(#{obj_ref(actor)}, "")|)
      add_property(actor, 'incarnation_id', '', %Q|{#{obj_ref(player)}, "r"}|)
      assert_equal 0, evaluate(%Q|valid(#{obj_ref(actor)}, "")|)
      set(actor, 'incarnation_id', 0)
      assert_equal 0, evaluate(%Q|valid(#{obj_ref(actor)}, "epoch")|)
      recycle(actor)
      @sock.close
    end
  end

  def test_private_identity_read_permissions_are_preserved
    actor = nil
    run_test_as('wizard') do
      actor = create(NOTHING)
      add_property(actor, 'incarnation_id', 'private-epoch', %Q|{#{obj_ref(player)}, ""}|)
      assert_equal 1, evaluate(%Q|valid(#{obj_ref(actor)}, "private-epoch")|)
      @sock.close
    end
    run_test_as('programmer') do
      assert_equal 1, evaluate("valid(#{obj_ref(actor)})")
      assert_equal E_PERM, evaluate(%Q|valid(#{obj_ref(actor)}, "private-epoch")|)
      @sock.close
    end
    run_test_as('wizard') { recycle(actor); @sock.close }
  end
end
