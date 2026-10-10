require 'test_helper'

class TestAst < Test::Unit::TestCase

  # Comparisons happen inside the server with equal(), which is exact about
  # case and map contents.  The helpers return what the server saw so that a
  # failure shows it.

  def lit(value)
    %Q|["type" -> "literal", "value" -> #{value}]|
  end

  def var(name)
    %Q|["type" -> "variable", "name" -> "#{name}"]|
  end

  def assert_ast(expected, *lines)
    code = value_ref(lines)
    actual = evaluate(%Q|equal(parse_ast(#{code}), #{expected}) \|\| toliteral(parse_ast(#{code}))|)
    assert_equal 1, actual, "AST of #{lines.inspect}"
  end

  # The AST of the single statement `<expression>;'.
  def assert_expr(expected, expression)
    assert_ast(%Q|{["type" -> "expr", "expr" -> #{expected}]}|, "#{expression};")
  end

  # `lines' must be laid out the way the server prints code.
  def assert_round_trip(*lines)
    code = value_ref(lines)
    actual = evaluate(%Q|equal(unparse_ast(parse_ast(#{code})), #{code}) \|\| toliteral(unparse_ast(parse_ast(#{code})))|)
    assert_equal 1, actual, "round trip of #{lines.inspect}"
  end

  def assert_unparses(expected_lines, ast)
    expected = value_ref(expected_lines)
    actual = evaluate(%Q|equal(unparse_ast(#{ast}), #{expected}) \|\| toliteral(unparse_ast(#{ast}))|)
    assert_equal 1, actual, "unparse of #{ast}"
  end

  # What unparse_ast() says is wrong with `ast'.
  def unparse_error(ast)
    simplify command %Q|; try unparse_ast(#{ast}); return "no error"; except e (E_INVARG) return e[2]; endtry|
  end

  def assert_invalid(message, ast)
    assert_equal "Invalid AST#{message}", unparse_error(ast)
    assert_equal 0, evaluate("validate_ast(#{ast})")
  end

  def expr_stmt(expr)
    %Q|{["type" -> "expr", "expr" -> #{expr}]}|
  end

  EVERYTHING = [
    'if (a)',
    '  x = 1;',
    'elseif (b)',
    '  x = 2;',
    'else',
    '  x = 3;',
    'endif',
    'for v in ({1, 2, @rest})',
    '  continue;',
    'endfor',
    'for v, k in (["a" -> 1, 2.5 -> #3])',
    '  break;',
    'endfor',
    'for i in [1..10]',
    'endfor',
    'while outer (x < 10)',
    '  while (1)',
    '    break outer;',
    '  endwhile',
    '  continue outer;',
    'endwhile',
    'fork (5)',
    '  player:tell("hi");',
    'endfork',
    'fork task (0)',
    'endfork',
    'try',
    '  x = this.(name);',
    'except e (E_PERM, @codes)',
    '  return e;',
    'except (ANY)',
    '  return;',
    'endtry',
    'try',
    '  $foo:(name)(@args);',
    'finally',
    '  x = $bar.baz;',
    'endtry',
    '{a, ?b = 2, ?c, @d} = args;',
    'x = `y[1] ! E_RANGE => 0\';',
    'y = `z ! ANY\';',
    'l[2..$] = {};',
    'l[^][1] = "a \\"quoted\\" string";',
    'z = x ? y | -z;',
    'return (1 + 2) * 3 ^ 2 % 4 == !x || y in l && ~z;'
  ]

  def test_that_a_program_is_a_list_of_statement_nodes
    run_test_as('programmer') do
      assert_ast('{}')
      assert_ast(%Q|{["type" -> "return", "value" -> #{lit 42}]}|, 'return 42;')
    end
  end

  def test_that_every_statement_of_a_sequence_is_kept
    run_test_as('programmer') do
      assert_equal 3, evaluate('length(parse_ast({"x = 1;", "y = 2;", "return x + y;"}))')
      assert_equal 2, evaluate('length(parse_ast({"if (1)", "x = 1;", "return x;", "endif"})[1]["arms"][1]["body"])')
    end
  end

  def test_literals
    run_test_as('programmer') do
      assert_expr(lit('1'), '1')
      assert_expr(lit('-1'), '-1')
      assert_expr(lit('1.5'), '1.5')
      assert_expr(lit('"one"'), '"one"')
      assert_expr(lit('#1'), '#1')
      assert_expr(lit('E_PERM'), 'E_PERM')
    end
  end

  def test_variables
    run_test_as('programmer') do
      assert_expr(var('x'), 'x')
      assert_expr(var('player'), 'player')
      assert_expr(var('true'), 'true')
    end
  end

  def test_binary_operators
    run_test_as('programmer') do
      ['+', '-', '*', '/', '%', '^', '&&', '||', '==', '!=', '<', '<=', '>',
       '>=', 'in', '|.', '&.', '^.', '<<', '>>'].each do |op|
        assert_expr(%Q|["type" -> "binary", "op" -> "#{op}", "lhs" -> #{var 'a'}, "rhs" -> #{var 'b'}]|,
                    "a #{op} b")
      end
    end
  end

  def test_unary_operators
    run_test_as('programmer') do
      ['-', '!', '~'].each do |op|
        assert_expr(%Q|["type" -> "unary", "op" -> "#{op}", "expr" -> #{var 'a'}]|, "#{op}a")
      end
    end
  end

  def test_properties
    run_test_as('programmer') do
      assert_expr(%Q|["type" -> "prop", "object" -> #{var 'a'}, "property" -> #{lit '"b"'}]|, 'a.b')
      assert_expr(%Q|["type" -> "prop", "object" -> #{var 'a'}, "property" -> #{var 'b'}]|, 'a.(b)')
      assert_expr(%Q|["type" -> "prop", "object" -> #{lit '#0'}, "property" -> #{lit '"b"'}]|, '$b')
    end
  end

  def test_verb_calls
    run_test_as('programmer') do
      assert_expr(%Q|["type" -> "verb_call", "object" -> #{var 'a'}, "verb" -> #{lit '"b"'}, "args" -> {}]|,
                  'a:b()')
      assert_expr(%Q|["type" -> "verb_call", "object" -> #{var 'a'}, "verb" -> #{var 'b'}, "args" -> {#{lit 1}, ["type" -> "splice", "expr" -> #{var 'c'}]}]|,
                  'a:(b)(1, @c)')
    end
  end

  def test_function_calls
    run_test_as('programmer') do
      assert_expr(%Q|["type" -> "call", "function" -> "length", "args" -> {#{var 'a'}}]|, 'length(a)')
      assert_expr(%Q|["type" -> "call", "function" -> "time", "args" -> {}]|, 'time()')
    end
  end

  def test_indexes_and_ranges
    run_test_as('programmer') do
      assert_expr(%Q|["type" -> "index", "base" -> #{var 'a'}, "index" -> #{lit 1}]|, 'a[1]')
      assert_expr(%Q|["type" -> "range", "base" -> #{var 'a'}, "from" -> ["type" -> "first"], "to" -> ["type" -> "last"]]|,
                  'a[^..$]')
    end
  end

  def test_lists_and_maps
    run_test_as('programmer') do
      assert_expr(%Q|["type" -> "list", "items" -> {}]|, '{}')
      assert_expr(%Q|["type" -> "list", "items" -> {#{lit 1}, ["type" -> "splice", "expr" -> #{var 'a'}]}]|,
                  '{1, @a}')
      assert_expr(%Q|["type" -> "map", "entries" -> {}]|, '[]')
      assert_expr(%Q|["type" -> "map", "entries" -> {["key" -> #{lit '"a"'}, "value" -> #{lit 1}], ["key" -> #{var 'b'}, "value" -> #{var 'c'}]}]|,
                  '["a" -> 1, b -> c]')
    end
  end

  def test_assignments
    run_test_as('programmer') do
      assert_expr(%Q|["type" -> "assign", "target" -> #{var 'a'}, "value" -> #{lit 1}]|, 'a = 1')
      assert_expr(%Q|["type" -> "assign", "target" -> ["type" -> "scatter", "items" -> {["kind" -> "required", "var" -> "a"], ["kind" -> "optional", "var" -> "b"], ["kind" -> "optional", "var" -> "c", "default" -> #{lit 1}], ["kind" -> "rest", "var" -> "d"]}], "value" -> #{var 'args'}]|,
                  '{a, ?b, ?c = 1, @d} = args')
    end
  end

  def test_conditional_and_catch_expressions
    run_test_as('programmer') do
      assert_expr(%Q|["type" -> "conditional", "condition" -> #{var 'a'}, "consequent" -> #{var 'b'}, "alternate" -> #{var 'c'}]|,
                  'a ? b | c')
      assert_expr(%Q|["type" -> "catch", "expr" -> #{var 'a'}]|, "`a ! ANY'")
      assert_expr(%Q|["type" -> "catch", "expr" -> #{var 'a'}, "codes" -> {#{lit 'E_PERM'}, ["type" -> "splice", "expr" -> #{var 'b'}]}, "default" -> #{lit 0}]|,
                  "`a ! E_PERM, @b => 0'")
    end
  end

  def test_if_statements
    run_test_as('programmer') do
      assert_ast(%Q|{["type" -> "if", "arms" -> {["condition" -> #{var 'a'}, "body" -> {}]}]}|,
                 'if (a)', 'endif')
      assert_ast(%Q|{["type" -> "if", "arms" -> {["condition" -> #{var 'a'}, "body" -> {["type" -> "return"]}], ["condition" -> #{var 'b'}, "body" -> {}]}, "else" -> {["type" -> "return", "value" -> #{lit 1}]}]}|,
                 'if (a)', 'return;', 'elseif (b)', 'else', 'return 1;', 'endif')
    end
  end

  def test_loops
    run_test_as('programmer') do
      assert_ast(%Q|{["type" -> "for", "var" -> "v", "expr" -> #{var 'l'}, "body" -> {["type" -> "break"]}]}|,
                 'for v in (l)', 'break;', 'endfor')
      assert_ast(%Q|{["type" -> "for", "var" -> "v", "index" -> "k", "expr" -> #{var 'l'}, "body" -> {}]}|,
                 'for v, k in (l)', 'endfor')
      assert_ast(%Q|{["type" -> "for_range", "var" -> "i", "from" -> #{lit 1}, "to" -> #{var 'n'}, "body" -> {["type" -> "continue"]}]}|,
                 'for i in [1..n]', 'continue;', 'endfor')
      assert_ast(%Q|{["type" -> "while", "condition" -> #{var 'a'}, "body" -> {}]}|,
                 'while (a)', 'endwhile')
      assert_ast(%Q|{["type" -> "while", "name" -> "w", "condition" -> #{var 'a'}, "body" -> {["type" -> "break", "name" -> "w"], ["type" -> "continue", "name" -> "w"]}]}|,
                 'while w (a)', 'break w;', 'continue w;', 'endwhile')
    end
  end

  def test_fork_statements
    run_test_as('programmer') do
      assert_ast(%Q|{["type" -> "fork", "delay" -> #{lit 0}, "body" -> {["type" -> "return"]}]}|,
                 'fork (0)', 'return;', 'endfork')
      assert_ast(%Q|{["type" -> "fork", "var" -> "t", "delay" -> #{lit 0}, "body" -> {}]}|,
                 'fork t (0)', 'endfork')
    end
  end

  def test_try_statements
    run_test_as('programmer') do
      assert_ast(%Q|{["type" -> "try_except", "body" -> {["type" -> "return"]}, "excepts" -> {["var" -> "e", "codes" -> {#{lit 'E_PERM'}}, "body" -> {}], ["body" -> {["type" -> "return", "value" -> #{lit 1}]}]}]}|,
                 'try', 'return;', 'except e (E_PERM)', 'except (ANY)', 'return 1;', 'endtry')
      assert_ast(%Q|{["type" -> "try_finally", "body" -> {["type" -> "return"]}, "finally" -> {}]}|,
                 'try', 'return;', 'finally', 'endtry')
    end
  end

  def test_that_code_round_trips
    run_test_as('programmer') do
      assert_round_trip()
      assert_round_trip('return;')
      assert_round_trip(*EVERYTHING)
    end
  end

  def test_that_an_ast_survives_a_round_trip
    run_test_as('programmer') do
      code = value_ref(EVERYTHING)
      assert_equal 1, evaluate("equal(parse_ast(unparse_ast(parse_ast(#{code}))), parse_ast(#{code}))")
    end
  end

  def test_that_unparse_ast_prints_what_verb_code_prints
    run_test_as('programmer') do
      o = create(:nothing)
      add_verb(o, [player, 'xd', 'test'], ['this', 'none', 'this'])
      set_verb_code(o, 'test') { |vc| vc.concat(EVERYTHING) }
      [['', ''], [', 1', ', 1'], [', 0, 0', ', 0, 0'], [', 1, 0', ', 1, 0']].each do |unparse_args, code_args|
        assert_equal 1, evaluate(%Q|equal(unparse_ast(verb_ast(#{o}, "test")#{unparse_args}), verb_code(#{o}, "test"#{code_args}))|)
      end
    end
  end

  def test_unparse_options
    run_test_as('programmer') do
      ast = 'parse_ast({"if (a)", "return 1 + 2 * 3;", "endif"})'
      assert_unparses(['if (a)', '  return 1 + 2 * 3;', 'endif'], ast)
      assert_unparses(['if (a)', '  return 1 + (2 * 3);', 'endif'], "#{ast}, 1")
      assert_unparses(['if (a)', 'return 1 + 2 * 3;', 'endif'], "#{ast}, 0, 0")
    end
  end

  def test_that_verb_ast_is_the_ast_of_the_verb_code
    run_test_as('programmer') do
      o = create(:nothing)
      add_verb(o, [player, 'xd', 'test'], ['this', 'none', 'this'])
      set_verb_code(o, 'test') { |vc| vc.concat(EVERYTHING) }
      assert_equal 1, evaluate(%Q|equal(verb_ast(#{o}, "test"), parse_ast(#{value_ref(EVERYTHING)}))|)
      assert_equal 1, evaluate(%Q|equal(verb_ast(#{o}, 1), verb_ast(#{o}, "test"))|)
    end
  end

  def test_that_verb_ast_checks_its_arguments_like_verb_code
    o = nil
    run_test_as('programmer') do
      o = create(:nothing)
      add_verb(o, [player, 'rxd', 'readable'], ['this', 'none', 'this'])
      add_verb(o, [player, 'xd', 'unreadable'], ['this', 'none', 'this'])
      assert_equal E_TYPE, evaluate('verb_ast(1, "test")')
      assert_equal E_TYPE, evaluate("verb_ast(#{o}, {})")
      assert_equal E_INVARG, evaluate('verb_ast(#-1, "test")')
      assert_equal E_VERBNF, evaluate(%Q|verb_ast(#{o}, "missing")|)
      assert_equal E_ARGS, evaluate("verb_ast(#{o})")
    end
    run_test_as('programmer') do
      assert_equal 1, evaluate(%Q|equal(verb_ast(#{o}, "readable"), {})|)
      assert_equal E_PERM, evaluate(%Q|verb_ast(#{o}, "unreadable")|)
    end
  end

  def test_that_parse_ast_checks_its_arguments
    run_test_as('programmer') do
      assert_equal E_ARGS, evaluate('parse_ast()')
      assert_equal E_ARGS, evaluate('parse_ast({}, {})')
      assert_equal E_TYPE, evaluate('parse_ast("return 1;")')
      assert_equal E_TYPE, evaluate('parse_ast({"return 1;", 2})')
    end
  end

  def test_that_parse_ast_raises_the_parser_errors
    run_test_as('programmer') do
      assert_equal E_INVARG, evaluate('parse_ast({"return +;"})')
      assert_equal 1, simplify(command(%q|; try parse_ast({"x = 1;", "return +;"}); except e (E_INVARG) return equal(e[2..3], {"Line 2:  syntax error", {"Line 2:  syntax error"}}) \|\| toliteral(e[2..3]); endtry|))
      assert_equal 1, simplify(command(%q|; try parse_ast({"break;", "no_such_function();"}); except e (E_INVARG) return equal(e[3], {"Line 1:  No enclosing loop for `break' statement", "Line 2:  Unknown built-in function: no_such_function"}) \|\| toliteral(e[3]); endtry|))
    end
  end

  def test_that_unparse_ast_checks_its_arguments
    run_test_as('programmer') do
      assert_equal E_ARGS, evaluate('unparse_ast()')
      assert_equal E_ARGS, evaluate('unparse_ast({}, 0, 0, 0)')
      assert_equal E_TYPE, evaluate('unparse_ast(1)')
      assert_equal E_TYPE, evaluate('unparse_ast(["type" -> "return"])')
      assert_equal E_INVARG, evaluate('unparse_ast({1})')
    end
  end

  def test_that_optional_fields_can_be_left_out
    run_test_as('programmer') do
      assert_unparses(['return;'], '{["type" -> "return"]}')
      assert_unparses(['while (1)', '  break;', 'endwhile'],
                      %Q|{["type" -> "while", "condition" -> #{lit 1}, "body" -> {["type" -> "break"]}]}|)
      assert_unparses(['if (1)', 'endif'],
                      %Q|{["type" -> "if", "arms" -> {["condition" -> #{lit 1}, "body" -> {}]}]}|)
      assert_unparses(["`x ! ANY';"], expr_stmt(%Q|["type" -> "catch", "expr" -> #{var 'x'}]|))
      assert_unparses(['{?a} = args;'],
                      expr_stmt(%Q|["type" -> "assign", "target" -> ["type" -> "scatter", "items" -> {["kind" -> "optional", "var" -> "a"]}], "value" -> #{var 'args'}]|))
    end
  end

  def test_that_extra_fields_are_ignored
    run_test_as('programmer') do
      assert_unparses(['return 1;'],
                      '{["type" -> "return", "line" -> 12, "value" -> ["type" -> "literal", "value" -> 1, "note" -> {}]]}')
    end
  end

  def test_that_unparse_ast_adds_the_parentheses_a_tree_needs
    run_test_as('programmer') do
      sum = %Q|["type" -> "binary", "op" -> "+", "lhs" -> #{lit 1}, "rhs" -> #{lit 2}]|
      assert_unparses(['(1 + 2) * 3;'],
                      expr_stmt(%Q|["type" -> "binary", "op" -> "*", "lhs" -> #{sum}, "rhs" -> #{lit 3}]|))
      assert_unparses(['1 - (1 + 2);'],
                      expr_stmt(%Q|["type" -> "binary", "op" -> "-", "lhs" -> #{lit 1}, "rhs" -> #{sum}]|))
      assert_unparses(['(1 + 2).name;'],
                      expr_stmt(%Q|["type" -> "prop", "object" -> #{sum}, "property" -> #{lit '"name"'}]|))
      assert_unparses(['x.("not an identifier");', 'x.("if");'],
                      %Q|{["type" -> "expr", "expr" -> ["type" -> "prop", "object" -> #{var 'x'}, "property" -> #{lit '"not an identifier"'}]], ["type" -> "expr", "expr" -> ["type" -> "prop", "object" -> #{var 'x'}, "property" -> #{lit '"if"'}]]}|)
    end
  end

  def test_that_variable_names_ignore_case_like_the_parser
    run_test_as('programmer') do
      assert_unparses(['player = Foo;', 'return Foo;'],
                      %Q|{["type" -> "expr", "expr" -> ["type" -> "assign", "target" -> #{var 'PLAYER'}, "value" -> #{var 'Foo'}]], ["type" -> "return", "value" -> #{var 'foo'}]}|)
    end
  end

  def test_that_validate_ast_accepts_what_parse_ast_returns
    run_test_as('programmer') do
      assert_equal 1, evaluate('validate_ast({})')
      assert_equal 1, evaluate("validate_ast(parse_ast(#{value_ref(EVERYTHING)}))")
      assert_equal 0, evaluate('validate_ast(1)')
      assert_equal 0, evaluate('validate_ast(["type" -> "return"])')
      assert_equal E_ARGS, evaluate('validate_ast()')
    end
  end

  def test_that_a_malformed_ast_is_reported_with_its_position
    run_test_as('programmer') do
      assert_invalid(' at [1]: expected map, found integer', '{1}')
      assert_invalid(' at [1]: missing "type"', '{[]}')
      assert_invalid(' at [1]: missing "type"', '{["type" -> 1]}')
      assert_invalid(' at [2]: "bogus" is not a statement', '{["type" -> "return"], ["type" -> "bogus"]}')
      assert_invalid(' at [1]: "literal" is not a statement', "{#{lit 1}}")
      assert_invalid(' at [1]: missing "expr"', '{["type" -> "expr"]}')
      assert_invalid(' at [1].value: "return" is not an expression',
                     '{["type" -> "return", "value" -> ["type" -> "return"]]}')
      assert_invalid(' at [1].arms[2].condition.lhs: expected map, found string',
                     %Q|{["type" -> "if", "arms" -> {["condition" -> #{lit 1}, "body" -> {}], ["condition" -> ["type" -> "binary", "op" -> "+", "lhs" -> "x", "rhs" -> #{lit 1}], "body" -> {}]}]}|)
      assert_invalid(' at [1].arms[1].body[1]: missing "type"',
                     %Q|{["type" -> "if", "arms" -> {["condition" -> #{lit 1}, "body" -> {[]}]}]}|)
      assert_invalid(' at [1].body: expected list, found map',
                     %Q|{["type" -> "while", "condition" -> #{lit 1}, "body" -> ["type" -> "break"]]}|)
      assert_invalid(' at [1]: missing "body"',
                     %Q|{["type" -> "while", "condition" -> #{lit 1}]}|)
    end
  end

  def test_that_an_ast_must_be_something_the_parser_could_have_produced
    run_test_as('programmer') do
      assert_invalid(' at [1].arms: must not be empty', '{["type" -> "if", "arms" -> {}]}')
      assert_invalid(' at [1].excepts: must not be empty',
                     '{["type" -> "try_except", "body" -> {}, "excepts" -> {}]}')
      assert_invalid(' at [1].excepts[1].codes: must not be empty; leave it out to catch ANY',
                     '{["type" -> "try_except", "body" -> {}, "excepts" -> {["codes" -> {}, "body" -> {}]}]}')
      assert_invalid(' at [1].expr.name: expected a variable name', expr_stmt(var('1x')))
      assert_invalid(' at [1].expr.name: expected a variable name', expr_stmt(var('while')))
      assert_invalid(' at [1].expr.name: expected a variable name', expr_stmt('["type" -> "variable", "name" -> 1]'))
      assert_invalid(' at [1].var: expected a variable name',
                     %Q|{["type" -> "for", "var" -> "a b", "expr" -> #{var 'l'}, "body" -> {}]}|)
      assert_invalid(' at [1].expr.value: a value of type list cannot be written as a literal', expr_stmt(lit('{}')))
      assert_invalid(' at [1].expr.value: a value of type map cannot be written as a literal', expr_stmt(lit('[]')))
      assert_invalid(' at [1].expr.value: a value of type bool cannot be written as a literal', expr_stmt(lit('true')))
      assert_invalid(' at [1].expr.op: not a binary operator',
                     expr_stmt(%Q|["type" -> "binary", "op" -> "!", "lhs" -> #{lit 1}, "rhs" -> #{lit 1}]|))
      assert_invalid(' at [1].expr.op: not a unary operator',
                     expr_stmt(%Q|["type" -> "unary", "op" -> "+", "expr" -> #{lit 1}]|))
      assert_invalid(' at [1].expr.function: not a built-in function',
                     expr_stmt('["type" -> "call", "function" -> "no_such_function", "args" -> {}]'))
      assert_invalid(' at [1].expr.target: cannot be assigned to',
                     expr_stmt(%Q|["type" -> "assign", "target" -> #{lit 1}, "value" -> #{lit 1}]|))
      assert_invalid(' at [1].expr.target: cannot be assigned to',
                     expr_stmt(%Q|["type" -> "assign", "target" -> ["type" -> "conditional", "condition" -> #{var 'a'}, "consequent" -> #{var 'b'}, "alternate" -> #{var 'c'}], "value" -> #{lit 1}]|))
      assert_invalid(' at [1].expr: a scatter can only be the target of an assignment',
                     expr_stmt('["type" -> "scatter", "items" -> {}]'))
      assert_invalid(' at [1].expr: a splice can only appear in an argument list or a list',
                     expr_stmt(%Q|["type" -> "splice", "expr" -> #{var 'a'}]|))
      assert_invalid(' at [1].expr.target.items[1].kind: expected "required", "optional" or "rest"',
                     expr_stmt(%Q|["type" -> "assign", "target" -> ["type" -> "scatter", "items" -> {["kind" -> "maybe", "var" -> "a"]}], "value" -> #{var 'args'}]|))
      assert_invalid(' at [1].expr.target.items[1]: only an optional target can have a default',
                     expr_stmt(%Q|["type" -> "assign", "target" -> ["type" -> "scatter", "items" -> {["kind" -> "required", "var" -> "a", "default" -> #{lit 1}]}], "value" -> #{var 'args'}]|))
    end
  end

  def test_that_an_ast_cannot_nest_too_deeply
    run_test_as('programmer') do
      nest = %Q|e = #{lit 1}; for i in [1..%d] e = ["type" -> "unary", "op" -> "!", "expr" -> e]; endfor ast = {["type" -> "expr", "expr" -> e]};|
      assert_equal 1, simplify(command("; #{nest % 998} return validate_ast(ast) && length(unparse_ast(ast));"))
      assert_equal 0, simplify(command("; #{nest % 999} return validate_ast(ast);"))
      message = simplify(command("; #{nest % 999} try unparse_ast(ast); except e (E_INVARG) return e[2]; endtry"))
      assert_match(/\AInvalid AST at \[1\]\.expr\.expr.*\.\.\.: nested more than 1000 levels deep\z/, message)
    end
  end

end
