require 'test_helper'

# Fixed native allocation workload, identical on old and candidate binaries.
# Native ticks exclude the final scheduling boundary; process CPU includes free.
class TestWaifOwnerCountBenchmark < Test::Unit::TestCase
  def test_allocation_workload
    run_test_as('wizard') do
      klass = create(:waif)
      add_verb(klass, [player, 'xd', 'allocation_benchmark'], ['this', 'none', 'this'])
      set_verb_code(klass, 'allocation_benchmark') do |vc|
        vc << 'start = ticks_left();'
        vc << 'for i in [1..1000]'
        vc << 'w = this:new();'
        vc << 'w = 0;'
        vc << 'endfor'
        vc << 'used = start - ticks_left();'
        vc << 'suspend(0);'
        vc << 'return used;'
      end
      ticks = Array.new(120) { call(klass, 'allocation_benchmark') }
      assert(ticks.all? { |value| value.is_a?(Integer) })
      assert_equal 1, ticks.uniq.length
      puts "allocation_ticks_per_1000=#{ticks.first}; builtin_count=#{evaluate('length(function_info())')}"
    end
  end
end
