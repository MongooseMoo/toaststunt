#!/usr/bin/env ruby

require 'fileutils'
require 'socket'
require 'timeout'

class JsonV20CannedRoundTrip
  def initialize
    @root = File.expand_path('..', __dir__)
    @moo = File.join(@root, 'build', 'moo')
    @fixtures = File.join(__dir__, 'tests')
    @port = 9897
    @tmp = '/tmp/json-v20-canned'
  end

  def run
    cleanup
    FileUtils.mkdir_p(@tmp)
    run_anon_fixtures
    run_suspended_fixture
  ensure
    cleanup
  end

  private

  def cleanup
    FileUtils.rm_rf(@tmp)
  end

  def fixture(name)
    File.join(@fixtures, name)
  end

  def run_anon_fixtures
    %w[Anon1.db Anon2.db Anon3.db Anon4.db Anon5.db Anon6.db].each do |name|
      base = File.join(@tmp, File.basename(name, '.db'))
      json = "#{base}.v20"
      native = "#{base}.native.db"
      reload = "#{base}.reload.db"

      run_to_dump(fixture(name), native, ['--dump-json-v20', json], "#{base}.dump.log")
      raise "#{name}: JSON-v20 dump missing" unless File.directory?(json)
      raise "#{name}: native side dump missing" unless File.exist?(native)

      run_to_dump(json, reload, [], "#{base}.reload.log")
      raise "#{name}: JSON-v20 reload native dump missing" unless File.exist?(reload)
    end
  end

  def run_suspended_fixture
    base = File.join(@tmp, 'Suspended')
    json = "#{base}.v20"
    native = "#{base}.native.db"
    reload = "#{base}.reload.db"

    run_until_listening_then_stop(fixture('Suspended.db'), native,
                                  ['--dump-json-v20', json],
                                  "#{base}.dump.log")
    queued_json = File.join(json, 'tasks', 'queued.json')
    raise 'Suspended.db: queued task JSON missing' unless File.exist?(queued_json)
    raise 'Suspended.db: suspended task not present in JSON payload' unless File.read(queued_json).include?('1 suspended tasks')

    run_until_listening_then_stop(json, reload, [], "#{base}.reload.log")
    raise 'Suspended.db: JSON reload native dump missing' unless File.exist?(reload)
    raise 'Suspended.db: suspended task not present after JSON reload' unless File.read(reload).include?("1 suspended tasks\n")
  end

  def run_to_dump(input, output, extra_args, log)
    pid = spawn(@moo, input, output, *extra_args, @port.to_s, out: log, err: [:child, :out])
    Timeout.timeout(30) do
      _, status = Process.wait2(pid)
      raise "#{input}: moo exited with #{status.exitstatus}" unless status.success?
    end
  rescue Timeout::Error
    Process.kill('TERM', pid)
    Process.wait(pid)
  end

  def run_until_listening_then_stop(input, output, extra_args, log)
    pid = spawn(@moo, input, output, *extra_args, @port.to_s, out: log, err: [:child, :out])
    wait_for_port(pid)
    Process.kill('TERM', pid)
    _, status = Process.wait2(pid)
    raise "#{input}: moo exited with #{status.exitstatus}" unless status.success?
  rescue Exception
    begin
      Process.kill('TERM', pid) if pid
      Process.wait(pid) if pid
    rescue Errno::ESRCH, Errno::ECHILD
    end
    raise
  end

  def wait_for_port(pid)
    Timeout.timeout(15) do
      loop do
        begin
          TCPSocket.open('localhost', @port).close
          return
        rescue Errno::ECONNREFUSED
          raise 'moo exited before accepting connections' if Process.wait(pid, Process::WNOHANG)
          sleep 0.1
        end
      end
    end
  end
end

JsonV20CannedRoundTrip.new.run
