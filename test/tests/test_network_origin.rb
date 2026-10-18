require 'test_helper'
require 'timeout'
$stdout.sync = true

class TestNetworkOrigin < Test::Unit::TestCase
  def setup
    options['verbose'] = ENV['ORIGIN_DEBUG'] == '1'
  end

  def command(line)
    Timeout.timeout(5) { super }
  end

  def run_test_as(*account)
    super(*account) do
      # The helpers pipeline connect and their first command. That first input
      # legitimately retains the negative binding captured before native login.
      evaluate('player')
      yield
    ensure
      @sock.close if @sock && !@sock.closed?
    end
  end
  def tagged(tag)
    Timeout.timeout(5) do
      loop do
        line = @sock.gets
        raise 'socket closed before native probe' unless line
        puts '< ' + line if options['verbose']
        position = line.index(tag + ' ')
        next unless position
        return general_expression_parse('{0,' + line[(position + tag.length + 1)..-1].strip + '}')
      end
    end
  end

  def install_handler(name, code)
    assert_kind_of Integer, add_verb(SYSTEM, [player, 'xd', name], ['this', 'none', 'this'])
    assert_equal [], set_verb_code(SYSTEM, name, code)
  end

  def assert_socket_closed(socket)
    assert_raise(EOFError, Errno::ECONNRESET) do
      Timeout.timeout(5) { loop { socket.readpartial(1024) } }
    end
  end

  def with_oob(code = ['notify(player, "ORIGIN " + toliteral(task_network_origin()));'])
    install_handler('do_out_of_band_command', code)
    yield
  ensure
    delete_verb(SYSTEM, 'do_out_of_band_command') if @sock && !@sock.closed?
  end

  def test_literal_current_and_original_shape_permissions_and_copy_isolation
    run_test_as('programmer') do
      current, origin, actor = evaluate('{connection_binding(player), task_network_origin(), player}')
      assert_equal 2, current.length
      binding = current[0]
      assert_match(/\A[0-9a-f]{32}\z/, binding[0])
      assert_operator binding[1], :>, 0
      assert_operator binding[2], :>, 1
      assert_equal actor, binding[3]
      assert_equal [binding, current[1], 'inband', 0, SYSTEM, options['port'], 0], origin
      assert_equal E_PERM, evaluate('connection_binding(#0)')
      assert_equal 1, eval('a = connection_binding(player); a[1][1] = "forged"; return connection_binding(player)[1][1] != "forged"')
      assert_equal E_ARGS, evaluate('task_network_origin(1)')
    end
    run_test_as('wizard') { assert_equal 0, evaluate('connection_binding(#-999999)') }
  end

  def test_switch_keeps_original_origin_and_mints_epoch_on_the_same_physical_socket
    run_test_as('wizard') do
      original = evaluate('player')
      other = eval('o = create($nothing); move(o, player.location); set_player_flag(o, 1); o.programmer = 1; o.wizard = 1; return o')
      before = evaluate('connection_binding(player)[1]')
      send_string %Q|; old = task_network_origin(); switch_player(player, #{other}); notify(#{other}, "SWITCH " + toliteral({old, task_network_origin(), connection_binding(#{other})}));|
      old_origin, after_origin, current = tagged('SWITCH')
      assert_equal old_origin, after_origin
      assert_equal before, old_origin[0]
      assert_equal before[0..1], current[0][0..1]
      assert_equal before[2] + 1, current[0][2]
      assert_equal other, current[0][3]
      send_string %Q|; switch_player(player, #{original}); notify(#{original}, "BACK " + toliteral(connection_binding(#{original})));|
      current = tagged('BACK')
      assert_equal before[2] + 2, current[0][2]
      assert_equal before[0..1], current[0][0..1]
    end
  end

  def test_task_local_cannot_forge_origin_and_suspension_keeps_the_original
    run_test_as('wizard') do
      values = eval('a = task_network_origin(); set_task_local({"forged"}); suspend(0); return {a, task_network_origin()}')
      assert_equal values[0], values[1]
    end
  end

  def test_fork_has_no_raw_origin_and_parent_keeps_its_own
    run_test_as('wizard') do
      send_string '; a = task_network_origin(); fork (0); notify(player, "FORK " + toliteral(task_network_origin())); endfork; notify(player, "PARENT " + toliteral(a));'
      parent = tagged('PARENT')
      assert_equal 'inband', parent[2]
      assert_equal 0, tagged('FORK')
    end
  end

  def test_read_uses_the_outer_vm_origin_instead_of_the_consumed_line
    run_test_as('wizard') do
      send_string '; a = task_network_origin(); notify(player, "READING " + toliteral(a)); value = read(player); notify(player, "READBACK " + toliteral({a, task_network_origin(), value}));'
      original = tagged('READING')
      send_string 'second physical input'
      before, after, value = tagged('READBACK')
      assert_equal original, before
      assert_equal before, after
      assert_equal 'second physical input', value
    end
  end

  def test_real_telnet_typed_prefix_and_synthetic_oob_are_distinct
    run_test_as('wizard') do
      with_oob do
        @sock.write([255, 251, 201].pack('C*'))
        real = tagged('ORIGIN')
        assert_equal 'oob', real[2]
        assert_equal 1, real[3]
        assert_equal options['port'], real[5], 'birth local port is listener port, not client peer port'
        send_string '#$# typed'
        typed = tagged('ORIGIN')
        assert_equal ['oob', 0], typed[2..3]
        assert_operator typed[1], :>, real[1]
        assert_equal 1, eval('force_input(player, "#$# forced"); return 1')
        assert_equal 0, tagged('ORIGIN')
      end
    end
  end

  def test_telnet_subnegotiation_fragments_do_not_create_partial_origins
    run_test_as('wizard') do
      with_oob do
        head = evaluate('connection_binding(player)[2]')
        @sock.write([255, 250, 201].pack('C*') + 'Core.Test {"part":')
        assert_nil IO.select([@sock], nil, nil, 0.02)
        @sock.write('1}' + [255, 240].pack('C*'))
        origin = tagged('ORIGIN')
        assert_equal head + 1, origin[1]
        assert_equal ['oob', 1], origin[2..3]
      end
    end
  end

  def test_disabled_oob_and_binary_dispatch_never_claim_oob
    run_test_as('wizard') do
      install_handler('do_command', ['if (argstr[1] == ";"); return 0; endif',
        'set_connection_option(player, "binary", 0);',
        'notify(player, "DISPATCH " + toliteral(task_network_origin())); return 1;'])
      begin
        set_connection_option(player, 'disable-oob', 1)
        @sock.write([255, 251, 201].pack('C*'))
        origin = tagged('DISPATCH')
        assert_equal ['inband', 1], origin[2..3]
        set_connection_option(player, 'disable-oob', 0)
        # Binary output omits the helper protocol's line separators. Supply an
        # explicit raw newline for the handshake instead of waiting for them.
        send_string '; set_connection_option(player, "binary", 1); notify(player, "BINARY_READY 1~0A");'
        assert_equal 1, tagged('BINARY_READY')
        @sock.write('raw bytes')
        origin = tagged('DISPATCH')
        assert_equal ['binary', 0], origin[2..3]
      ensure
        delete_verb(SYSTEM, 'do_command')
      end
    end
  end

  def test_background_native_wait_keeps_the_same_original
    run_test_as('wizard') do
      values = eval('a = task_network_origin(); set_thread_mode(1); connection_name_lookup(player, 0); return {a, task_network_origin()}')
      assert_equal values[0], values[1]
    end
  end

  def test_nested_native_error_callback_and_its_suspension_have_no_origin
    run_test_as('wizard') do
      install_handler('handle_uncaught_error', ['notify(player, "ERROR_ORIGIN " + toliteral(task_network_origin()));',
        'suspend(0); notify(player, "ERROR_RESUMED " + toliteral(task_network_origin())); return 1;'])
      install_handler('do_command', ['if (argstr == "origin-error"); raise(E_DIV); endif; return 0;'])
      begin
        send_string 'origin-error'
        assert_equal 0, tagged('ERROR_ORIGIN')
        assert_equal 0, tagged('ERROR_RESUMED')
        assert_equal 'inband', evaluate('task_network_origin()[3]')
      ensure
        delete_verb(SYSTEM, 'do_command')
        delete_verb(SYSTEM, 'handle_uncaught_error')
      end
    end
  end

  def test_http_reader_keeps_its_outer_origin
    run_test_as('wizard') do
      send_string '; a = task_network_origin(); set_connection_option(player, "hold-input", 1); set_connection_option(player, "binary", 1); notify(player, "HTTP_READY 1~0A"); value = read_http("request", player); set_connection_option(player, "binary", 0); set_connection_option(player, "hold-input", 0); notify(player, "HTTP_RESULT " + toliteral({a, task_network_origin(), value["method"]}));'
      assert_equal 1, tagged('HTTP_READY')
      @sock.write("GET /origin HTTP/1.1\r\nHost: local\r\n\r\n")
      before, after, method = tagged('HTTP_RESULT')
      assert_equal before, after
      assert_equal 'GET', method
    end
  end

  def test_nested_native_lag_callback_and_its_suspension_have_no_origin
    run_test_as('wizard') do
      eval('add_property($server_options, "task_lag_threshold", 0.1, {player, "r"}); add_property($server_options, "fg_ticks", 2147483647, {player, "r"}); load_server_options(); return 1')
      install_handler('handle_lagging_task', ['notify(player, "LAG_ORIGIN " + toliteral(task_network_origin()));',
        'suspend(0); notify(player, "LAG_RESUMED " + toliteral(task_network_origin()));'])
      install_handler('do_command', ['if (argstr != "origin-lag"); return 0; endif',
        'start = ftime(1); while (ftime(1) - start < 0.12); endwhile',
        'notify(player, "LAG_PARENT " + toliteral(task_network_origin())); return 1;'])
      begin
        send_string 'origin-lag'
        assert_equal 'inband', tagged('LAG_PARENT')[2]
        assert_equal 0, tagged('LAG_ORIGIN')
        assert_equal 0, tagged('LAG_RESUMED')
      ensure
        delete_verb(SYSTEM, 'do_command')
        delete_verb(SYSTEM, 'handle_lagging_task')
        eval('delete_property($server_options, "task_lag_threshold"); delete_property($server_options, "fg_ticks"); load_server_options(); return 1')
      end
    end
  end

  def test_queued_physical_input_retains_old_binding_after_an_external_switch
    run_test_as('programmer') do
      actor = evaluate('player')
      before = evaluate('connection_binding(player)')
      set_connection_option(actor, 'hold-input', 1)
      physical = @sock
      send_string '; notify(player, "QUEUED " + toliteral({task_network_origin(), connection_binding(player)}));'
      @sock = TCPSocket.open(options['host'], options['port'])
      controller = @sock
      begin
        send_string 'connect wizard'
        evaluate('player')
        Timeout.timeout(5) do
          until evaluate("connection_binding(#{actor})[2]") > before[1] + 1
            Thread.pass
          end
        end
        other = eval("o = create($nothing); move(o, #{actor}.location); set_player_flag(o, 1); o.programmer = 1; return o")
        assert_equal 1, eval("switch_player(#{actor}, #{other}); set_connection_option(#{other}, \"hold-input\", 0); return 1")
        @sock = physical
        original, current = tagged('QUEUED')
        assert_equal before[0], original[0]
        assert_equal before[0][0..1], current[0][0..1]
        assert_equal before[0][2] + 1, current[0][2]
        assert_equal other, current[0][3]
      ensure
        controller.close unless controller.closed?
        @sock = physical
      end
    end
  end

  def test_real_two_socket_redirect_publishes_the_incoming_physical_binding
    run_test_as('programmer') do
      target = evaluate('player')
      old_binding = evaluate('connection_binding(player)[1]')
      displaced = @sock
      @sock = TCPSocket.open(options['host'], options['port'])
      incoming = @sock
      begin
        send_string 'connect wizard'
        evaluate('player')
        before = evaluate('connection_binding(player)[1]')
        send_string "; switch_player(player, #{target}); notify(#{target}, \"REDIRECT \" + toliteral(connection_binding(#{target})));"
        current = tagged('REDIRECT')
        assert_equal before[0..1], current[0][0..1]
        assert_not_equal old_binding[1], current[0][1]
        assert_equal before[2] + 1, current[0][2]
        assert_equal target, current[0][3]
        assert_socket_closed(displaced)
      ensure
        displaced.close unless displaced.closed?
        @sock = incoming
      end
    end
  end

  def test_booted_negative_return_login_preserves_connected_notifier_with_no_receipt
    run_test_as('wizard') do
      controller = evaluate('player')
      target = eval('o = create($nothing); set_player_flag(o, 1); return o')
      original_login = evaluate('verb_code(#0, "do_login_command")')
      install_handler('user_connected', ["notify(#{controller}, \"BOOT_RETURN \" + toliteral({player, connection_binding(player), task_network_origin()}));"])
      begin
        assert_equal [], set_verb_code(SYSTEM, 'do_login_command', ['boot_player(player);', "return #{target};"])
        booted = TCPSocket.open(options['host'], options['port'])
        booted.puts 'boot-and-return-native'
        assert_equal [target, 0, 0], tagged('BOOT_RETURN')
        assert_socket_closed(booted)
      ensure
        booted.close if booted && !booted.closed?
        set_verb_code(SYSTEM, 'do_login_command', original_login)
        delete_verb(SYSTEM, 'user_connected')
      end
    end
  end
end
