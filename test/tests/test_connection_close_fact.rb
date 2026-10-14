require 'test_helper'

class TestConnectionCloseFact < Test::Unit::TestCase
  def prepare_clock(identity, permissions = '')
    owner = %Q|{#{obj_ref(player)}, ""}|
    add_property(player, 'incarnation_id', identity, owner)
    add_property(player, '_connection_closed', 0, %Q|{#{obj_ref(player)}, "#{permissions}"}|)
    add_property(player, 'cloaked', 1, owner)
    add_property(player, 'last_disconnect_time', 0, owner)
    player
  end

  def remove_clock(actor)
    ['_connection_closed', 'incarnation_id', 'cloaked', 'last_disconnect_time'].each do |name|
      delete_property(actor, name)
    end
  end

  def test_client_close_records_private_incarnation_and_time_without_public_presence
    before = nil
    actor = nil
    run_test_as('wizard') do
      actor = prepare_clock('client-close-incarnation')
      before = evaluate('time()')
      @sock.close
    end
    run_test_as('wizard') do
      closed = get(actor, '_connection_closed')
      assert_equal 'client-close-incarnation', closed[0]
      assert_operator closed[1], :>=, before
      assert_operator closed[1], :<=, evaluate('time()')
      assert_equal 0, get(actor, 'last_disconnect_time')
      set(actor, 'incarnation_id', 'renewed-incarnation')
      assert_equal 'client-close-incarnation', get(actor, '_connection_closed')[0],
                   'the saved fact captures the closed lifetime rather than a mutable identity'
      remove_clock(actor)
      @sock.close
    end
  end

  def test_server_boot_records_the_same_private_fact
    actor = nil
    run_test_as('wizard') do
      actor = prepare_clock('server-close-incarnation')
      send_string ';return boot_player(player);'
      @sock.read
    end
    run_test_as('wizard') do
      assert_equal 'server-close-incarnation', get(actor, '_connection_closed')[0]
      assert_equal 0, get(actor, 'last_disconnect_time')
      remove_clock(actor)
      @sock.close
    end
  end

  def test_a_public_property_is_not_used_to_publish_the_close_time
    actor = nil
    run_test_as('wizard') { actor = prepare_clock('public-property', 'r'); @sock.close }
    run_test_as('wizard') do
      assert_equal 0, get(actor, '_connection_closed')
      remove_clock(actor)
      @sock.close
    end
  end

  def test_a_writable_property_is_not_an_authoritative_close_fact
    actor = nil
    run_test_as('wizard') { actor = prepare_clock('writable-property', 'w'); @sock.close }
    run_test_as('wizard') do
      assert_equal 0, get(actor, '_connection_closed')
      remove_clock(actor)
      @sock.close
    end
  end

  def test_a_nonwizard_owned_property_is_not_an_authoritative_close_fact
    actor = nil
    run_test_as('programmer') { actor = prepare_clock('nonwizard-property'); @sock.close }
    run_test_as('wizard') do
      assert_equal 0, get(actor, '_connection_closed')
      remove_clock(actor)
      @sock.close
    end
  end
end
