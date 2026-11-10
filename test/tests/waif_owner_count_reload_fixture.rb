require 'test_helper'

class TestWaifOwnerCountReload < Test::Unit::TestCase
  def test_prepare_old_checkpoint
    run_test_as('wizard') do
      first = create(:object)
      second = create(:object)
      klass = create(:waif)
      [first, second].each_with_index do |owner, i|
        add_verb(klass, [owner, 'xd', "allocate#{i}"], ['this', 'none', 'this'])
        set_verb_code(klass, "allocate#{i}") { |vc| vc << 'return this:new();' }
      end
      add_property(SYSTEM, 'owner_count_fixture', {}, [player, ''])
      add_verb(SYSTEM, [player, 'xd', 'owner_count_resume'], ['this', 'none', 'this'])
      set_verb_code(SYSTEM, 'owner_count_resume') do |vc|
        vc << 'suspend();'
        vc << '#0.owner_count_fixture["resumed"] = {sqrt(4), waif_stats()["total"], simplex_noise({0.0, 0.0})};'
      end
      add_verb(SYSTEM, [player, 'xd', 'owner_count_probe'], ['this', 'none', 'this'])
      set_verb_code(SYSTEM, 'owner_count_probe') { |vc| vc << 'return simplex_noise({0.0, 0.0});' }
      assert_equal 0.0, call(SYSTEM, 'owner_count_probe')
      assert_outbound_disabled_probe_error
      assert_equal [4, 1], simplify(command(%Q|; x = #{klass}:allocate0(); y = #{klass}:allocate0(); z = #{klass}:allocate1(); #0.owner_count_fixture = ["owners" -> {#{first}, #{second}}, "refs" -> {x, x, y, z}, "resumed" -> 0]; fork t (0) #0:owner_count_resume(); endfork; #0.owner_count_fixture["task"] = t; suspend(0); return {length(#0.owner_count_fixture["refs"]), length(queued_tasks())};|))
    end
  end

  def test_verify_first_reload
    run_test_as('wizard') do
      assert_outbound_disabled_probe_error
      assert_equal 0.0, call(SYSTEM, 'owner_count_probe')
      assert_equal [2, 1, 1, [2.0, 3, 0.0], 1], simplify(command('; f = #0.owner_count_fixture; resume(f["task"], 0); suspend(0); return {waif_owner_count(f["owners"][1]), waif_owner_count(f["owners"][2]), equal(f["refs"][1], f["refs"][2]), #0.owner_count_fixture["resumed"], length(function_info()) < 256};'))
    end
  end

  def test_verify_second_reload_and_release
    run_test_as('wizard') do
      assert_equal [[2, 1], [2, 1], [0, 0]], simplify(command('; f = #0.owner_count_fixture; a = f["owners"][1]; b = f["owners"][2]; loaded = {waif_owner_count(a), waif_owner_count(b)}; #0.owner_count_fixture["refs"] = {}; f = 0; pending = {waif_owner_count(a), waif_owner_count(b)}; suspend(0); return {loaded, pending, {waif_owner_count(a), waif_owner_count(b)}};'))
    end
  end

  private

  def assert_outbound_disabled_probe_error
    # Existing Test.db eval returns the native tuple; add no MOO error catch.
    error = general_expression_parse(command('; return url_decode("a%20b");'))
    assert_equal [E_PERM, 'Outbound network connections are disabled.', 0], error[0, 3]
  end
end
