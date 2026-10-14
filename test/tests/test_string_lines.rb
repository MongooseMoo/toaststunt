require 'test_helper'
require 'fileutils'

# Run only against a disposable Test.db with the existing isolated runner:
# python3 test/run_map_order_regressions.py build/moo --test test_string_lines.rb
class TestStringLines < Test::Unit::TestCase
  def test_validator_empty_singleton_valid_text_and_immutability
    run_test_as('programmer') do
      assert_equal 1, evaluate('validate_string_lines({})')
      assert_equal 1, evaluate('validate_string_lines({""})')
      rows = ['', 'Ab', '\\n', '\\r', 'one two', 'tab\there']
      assert_equal [1, 1], simplify(command("; rows = #{value_ref(rows)}; old = rows; return {validate_string_lines(rows), equal(rows, old)};"))
      assert_equal E_TYPE, evaluate('validate_string_lines("line")')
      assert_equal E_TYPE, evaluate('validate_string_lines({{}, "line"})')
    end
  end

  def test_validator_runtime_controls_and_ordered_mixed_invalid_rows
    run_test_as('wizard') do
      ['chr(10)', 'chr(13)', 'chr(13, 10)', '"x" + chr(10) + "y"'].each do |control|
        assert_equal E_INVARG, evaluate("validate_string_lines({#{control}})"), control
        assert_equal E_INVARG, evaluate("validate_string_lines({\"valid\", #{control}, 17})"), control
        assert_equal E_TYPE, evaluate("validate_string_lines({\"valid\", 17, #{control}})"), control
        assert_equal E_INVARG, evaluate("validate_string_lines({#{control}, {}})"), control
      end
    end
  end

  def test_validator_large_list_beyond_serialization_limit
    run_test_as('wizard') do
      evaluate('add_property($server_options, "max_concat_catchable", 1, {player, ""})')
      evaluate('add_property($server_options, "max_string_concat", 1021, {player, ""})')
      evaluate('load_server_options()')
      begin
        # 4096 rows, 64 characters each. This LIST remains within the native
        # LIST quota but cannot be serialized by toliteral under the limit.
        row = 'a' * 64
        build = "rows = {#{value_ref(row)}}; for i in [1..12]; rows = {@rows, @rows}; endfor;"
        assert_equal [1, 4096, 1], simplify(command("; #{build} old = rows; return {validate_string_lines(rows), length(rows), equal(rows, old)};"))
        assert_equal E_QUOTA, simplify(command("; #{build} return toliteral(rows);"))
        with_line_file('lines-large', 'w-tn') do |handle|
          assert_equal [0, 1], simplify(command("; #{build} old = rows; return {file_writelines(#{handle}, rows), equal(rows, old)};"))
        end
        assert_equal (row + "\n") * 4096, File.binread('files/lines-large')
        file_remove('lines-large')
      ensure
        evaluate('delete_property($server_options, "max_concat_catchable")')
        evaluate('delete_property($server_options, "max_string_concat")')
        evaluate('load_server_options()')
      end
    end
  end

  def test_writer_empty_list_is_an_empty_scalar_loop
    ['programmer', 'wizard'].each do |actor|
      run_test_as(actor) do
        assert_equal 0, evaluate('file_writelines(0, {})')
        assert_equal 0, evaluate('file_writelines("invalid handle type", {})')
        assert_equal 0, evaluate('file_writelines({}, {})')
        assert_equal E_TYPE, evaluate('file_writelines(0, "not a list")')
      end
    end
  end

  def test_writer_nonempty_scalar_argument_and_permission_order
    ['programmer', 'wizard'].each do |actor|
      run_test_as(actor) do
        [['"bad"', '17'], ['"bad"', '"line"'], ['0', '17'], ['0', '{}']].each do |handle, row|
          assert_equal scalar_error("file_writeline(#{handle}, #{row})"),
                       scalar_error("file_writelines(#{handle}, {#{row}})")
        end
        expected = actor == 'wizard' ? E_INVARG : E_PERM
        assert_equal expected, evaluate('file_writelines(0, {"line", 17})')
      end
    end
  end

  def test_normal_builtin_protection_precedes_even_empty_list_operations
    run_test_as('wizard') do
      evaluate('add_property($server_options, "protect_validate_string_lines", 1, {player, ""})')
      evaluate('add_property($server_options, "protect_file_writelines", 1, {player, ""})')
      evaluate('load_server_options()')
    end
    begin
      run_test_as('programmer') do
        assert_equal E_PERM, evaluate('validate_string_lines({})')
        assert_equal E_PERM, evaluate('file_writelines(0, {})')
      end
      run_test_as('wizard') do
        assert_equal 1, evaluate('validate_string_lines({})')
        assert_equal 0, evaluate('file_writelines(0, {})')
      end
    ensure
      run_test_as('wizard') do
        evaluate('delete_property($server_options, "protect_validate_string_lines")')
        evaluate('delete_property($server_options, "protect_file_writelines")')
        evaluate('load_server_options()')
      end
    end
  end

  def test_writer_read_only_and_closed_handles_keep_scalar_errors
    run_test_as('wizard') do
      with_line_file('lines-readonly', 'w-tn') { |handle| file_writeline(handle, 'before') }
      with_line_file('lines-readonly', 'r-tn') do |handle|
        assert_equal E_TYPE, evaluate("file_writelines(#{handle}, {17})")
        assert_equal scalar_error("file_writeline(#{handle}, \"after\")"),
                     scalar_error("file_writelines(#{handle}, {\"after\", 17})")
        assert_equal 0, evaluate("file_writelines(#{handle}, {})")
      end
      handle = file_open('lines-readonly', 'r-tn')
      file_close(handle)
      assert_equal scalar_error("file_writeline(#{handle}, \"after\")"),
                   scalar_error("file_writelines(#{handle}, {\"after\"})")
      file_remove('lines-readonly')
    end
  end

  def test_writer_ordinary_caller_cannot_use_a_valid_wizard_handle
    handle = nil
    run_test_as('wizard') do
      FileUtils.mkdir_p('files')
      handle = file_open('lines-permission', 'w-bf')
      assert_kind_of(Integer, handle)
    end
    run_test_as('programmer') do
      assert_equal E_PERM, evaluate("file_writelines(#{handle}, {\"first\", 17})")
      assert_equal E_PERM, evaluate("file_writelines(#{handle}, {\"bad~ZZ\", 17})")
      assert_equal E_TYPE, evaluate("file_writelines(#{handle}, {17, \"first\"})")
      assert_equal 0, evaluate("file_writelines(#{handle}, {})")
    end
    assert_equal '', File.binread('files/lines-permission')
  ensure
    run_test_as('wizard') do
      file_close(handle) if handle.kind_of?(Integer)
      file_remove('lines-permission')
    end
  end

  def test_writer_exact_bytes_text_binary_flush_and_append_modes
    run_test_as('wizard') do
      ['tn', 'tf', 'bn', 'bf'].each do |mode|
        [['', 'Ab', 'plain'], ['x~00discard', 'y~0Az', '~7E~FF']].each do |rows|
          ['scalar', 'bulk'].each do |kind|
            with_line_file("lines-#{kind}", "w-#{mode}") do |handle|
              if kind == 'scalar'
                rows.each { |row| assert_equal 0, file_writeline(handle, row) }
              else
                assert_equal 0, evaluate("file_writelines(#{handle}, #{value_ref(rows)})")
              end
            end
            with_line_file("lines-#{kind}", "a-#{mode}") do |handle|
              expression = kind == 'scalar' ? "file_writeline(#{handle}, \"end\")" : "file_writelines(#{handle}, {\"end\"})"
              assert_equal 0, evaluate(expression)
            end
          end
          # Independent expected bytes, including decoded NUL truncation.
          expected = rows.map { |row| mode[0] == 'b' ? row.binary_string_to_binary.split("\x00", 2).first.to_s : row }.join("\n") + "\nend\n"
          assert_equal expected, File.binread('files/lines-scalar')
          assert_equal expected, File.binread('files/lines-bulk')
          file_remove('lines-scalar')
          file_remove('lines-bulk')
        end
      end
    end
  end

  def test_writer_partial_files_for_later_type_and_filter_errors
    run_test_as('wizard') do
      ['w-bn', 'w-bf'].each do |mode|
        with_line_file('lines-partial', mode) do |handle|
          assert_equal E_TYPE, evaluate("file_writelines(#{handle}, {\"first\", 17, \"never\"})")
          assert_equal "first\n", File.binread('files/lines-partial') if mode == 'w-bf'
          assert_equal scalar_error("file_writeline(#{handle}, \"bad~ZZ\")"),
                       scalar_error("file_writelines(#{handle}, {\"bad~ZZ\", 17})")
        end
        assert_equal "first\n", File.binread('files/lines-partial')
        file_remove('lines-partial')
        with_line_file('lines-partial', mode) do |handle|
          assert_equal E_INVARG, evaluate("file_writelines(#{handle}, {\"first\", \"bad~ZZ\", 17})")
        end
        assert_equal "first\n", File.binread('files/lines-partial')
        file_remove('lines-partial')
      end
    end
  end

  def test_writer_flush_visibility_and_input_immutability
    run_test_as('wizard') do
      with_line_file('lines-flush', 'w-bf') do |handle|
        assert_equal [0, 1], simplify(command("; rows = {\"first\", \"second\"}; old = rows; return {file_writelines(#{handle}, rows), equal(rows, old)};"))
        assert_equal "first\nsecond\n", File.binread('files/lines-flush')
      end
      file_remove('lines-flush')
    end
  end

  def test_writer_io_error_precedes_later_type_error
    assert(File.exist?('/dev/full'), 'POSIX /dev/full required for native write error control')
    FileUtils.mkdir_p('files')
    File.symlink('/dev/full', 'files/lines-full')
    run_test_as('wizard') do
      row = 'x' * 16384
      expected = nil
      with_line_file('lines-full', 'w-bn') do |handle|
        expected = scalar_error("file_writeline(#{handle}, #{value_ref(row)})")
        assert_equal E_FILE, expected[0]
      end
      with_line_file('lines-full', 'w-bn') do |handle|
        assert_equal expected, scalar_error("file_writelines(#{handle}, {#{value_ref(row)}, 17})")
      end
      file_remove('lines-full')
    end
  ensure
    File.unlink('files/lines-full') if File.symlink?('files/lines-full')
  end

  def test_writer_keeps_ignored_flush_errors_and_next_row_order
    assert(File.exist?('/dev/full'), 'POSIX /dev/full required for native flush error control')
    FileUtils.mkdir_p('files')
    File.symlink('/dev/full', 'files/lines-full')
    run_test_as('wizard') do
      with_line_file('lines-full', 'w-bf') do |handle|
        assert_equal 0, file_writeline(handle, 'first')
        assert_equal 0, evaluate("file_writelines(#{handle}, {\"second\"})")
        assert_equal E_TYPE, evaluate("file_writelines(#{handle}, {\"third\", 17})")
      end
      file_remove('lines-full')
    end
  ensure
    File.unlink('files/lines-full') if File.symlink?('files/lines-full')
  end

  private

  def with_line_file(path, mode)
    FileUtils.mkdir_p('files')
    handle = file_open(path, mode)
    assert_kind_of(Integer, handle)
    begin
      yield handle
    ensure
      file_close(handle)
    end
  end

  def scalar_error(expression)
    # Use Test.db's existing eval boundary, adding no MOO catches. Compare
    # native error code/message/value; stack function names naturally differ.
    details = general_expression_parse(command("; return #{expression};"))
    assert_kind_of(Array, details)
    details[0, 3]
  end
end
