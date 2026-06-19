#!/usr/bin/env ruby

require 'fileutils'
require 'socket'
require 'timeout'

$LOAD_PATH.unshift(File.expand_path('tests/lib', __dir__))
require 'moo_support'

class JsonV20WaifRoundTrip
  include MooSupport

  def initialize
    @root = File.expand_path('..', __dir__)
    @moo = File.join(@root, 'build', 'moo')
    @input_db = File.join(__dir__, 'Test.db')
    @json_db = '/tmp/json-v20-waif.v20'
    @native_db = '/tmp/json-v20-waif-native.db'
    @reload_db = '/tmp/json-v20-waif-reload.db'
    @log = '/tmp/json-v20-waif.log'
    @reload_log = '/tmp/json-v20-waif-reload.log'
    @pid = nil
  end

  def run
    cleanup

    start_server(@input_db, @native_db, ['--dump-json-v20', @json_db], @log)
    setup_fixture
    wait_for_server_exit

    raise 'waifs.json was not dumped' unless File.exist?(File.join(@json_db, 'waifs.json'))

    start_server(@json_db, @reload_db, [], @reload_log)
    verify_fixture
    wait_for_server_exit
  ensure
    stop_server
  end

  private

  def cleanup
    FileUtils.rm_rf(@json_db)
    FileUtils.rm_f([@native_db, @reload_db, @log, @reload_log])
  end

  def start_server(input, output, extra_args, log)
    @pid = spawn(@moo, input, output, *extra_args, options['port'].to_s,
                 out: log, err: [:child, :out])
    wait_for_port
  end

  def wait_for_port
    Timeout.timeout(15) do
      loop do
        begin
          TCPSocket.open(options['host'], options['port']).close
          break
        rescue Errno::ECONNREFUSED
          raise 'moo exited before accepting connections' if process_exited?
          sleep 0.1
        end
      end
    end
  end

  def wait_for_server_exit
    Timeout.timeout(15) do
      Process.wait(@pid)
      @pid = nil
    end
  end

  def stop_server
    return unless @pid

    Process.kill('TERM', @pid)
    Process.wait(@pid)
  rescue Errno::ESRCH, Errno::ECHILD
  ensure
    @pid = nil
  end

  def process_exited?
    Process.wait(@pid, Process::WNOHANG)
  end

  def setup_fixture
    run_test_as('wizard') do
      waif_class = create(:waif)
      add_property(waif_class, ':data', {}, [player, ''])
      add_property(0, 'json_v20_waif', 0, [player, 'r'])

      result = simplify(command(%Q|; #0.json_v20_waif = #{waif_class}:new(); #0.json_v20_waif.data = ["outer" -> ["inner" -> "value"], "n" -> 42]; return {typeof(#0.json_v20_waif) == WAIF, #0.json_v20_waif.data["outer"]["inner"], #0.json_v20_waif.data["n"]};|))
      raise "unexpected setup result: #{result.inspect}" unless result == [1, 'value', 42]
    end
    close_socket
    shutdown_as_wizard
  end

  def verify_fixture
    run_test_as('wizard') do
      result = simplify(command(%Q|; return {typeof(#0.json_v20_waif) == WAIF, #0.json_v20_waif.data["outer"]["inner"], #0.json_v20_waif.data["n"]};|))
      raise "unexpected reload result: #{result.inspect}" unless result == [1, 'value', 42]
    end
    close_socket
    shutdown_as_wizard
  end

  def shutdown_as_wizard
    run_test_as('wizard') do
      shutdown
    end
    close_socket
  end

  def close_socket
    @sock.close if @sock && !@sock.closed?
  end
end

JsonV20WaifRoundTrip.new.run
