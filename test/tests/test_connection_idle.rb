require 'test_helper'
require 'timeout'

class TestConnectionIdle < Test::Unit::TestCase
  def test_out_of_band_input_preserves_idle_but_commands_reset_it
    run_test_as('wizard') do
      subject = @sock
      subject.setsockopt(Socket::IPPROTO_TCP, Socket::TCP_NODELAY, 1)
      who = me
      run_test_as('wizard') do
        begin
          evaluate('add_verb(#0, {player, "xd", "do_out_of_band_command"}, {"this", "none", "this"})')
          evaluate('set_verb_code(#0, "do_out_of_band_command", {"notify(player, \"OOB processed\");"})')
          baseline = evaluate("time() - idle_seconds(#{who})")
          # Wait only once for the server's one-second idle clock to advance.
          Timeout.timeout(2) do
            sleep(0.01) until evaluate("idle_seconds(#{who})") > 0
          end

          {
            'bodyless GMCP ping' => ["\xff\xfa\xc9Core.Ping\xff\xf0".b],
            'numeric GMCP ping' => ["\xff\xfa\xc9Core.Ping 100\xff\xf0".b],
            'fragmented GMCP ping' => ["\xff\xfa\xc9Core.".b, "Ping 100\xff\xf0".b],
            'MCP' => ["#$#dns-com-awns-ping 1 id: 1\r\n"],
            'telnet negotiation' => ["\xff\xfd\xc9".b],
          }.each do |name, fragments|
            fragments.each_with_index do |fragment, index|
              subject.write(fragment)
              # Let the server poll an incomplete subnegotiation before its tail.
              evaluate('1') if index < fragments.length - 1
            end
            Timeout.timeout(2) { loop { break if subject.gets&.strip == 'OOB processed' } }
            assert_equal baseline, evaluate("time() - idle_seconds(#{who})"), name
          end

          subject.write("; notify(player, \"command processed\");\r\n")
          Timeout.timeout(2) { loop { break if subject.gets&.strip == 'command processed' } }
          assert_operator evaluate("time() - idle_seconds(#{who})"), :>, baseline
        ensure
          evaluate('delete_verb(#0, "do_out_of_band_command")')
          @sock.close
          subject.close
        end
      end
    end
  end
end
