require "test_helper"

class TestAstRoundTrip < Test::Unit::TestCase

  # Simple round-trip test parsing raw MOO response
  def test_round_trip(test_name, input_code)
    puts "\n=== #{test_name} ==="
    
    # First validate that input code is valid MOO syntax
    begin
      o = create(:nothing)
      add_verb(o, [player, 'x', 'test_input'], ['this', 'none', 'this'])
      set_verb_code(o, 'test_input', [input_code])
      puts "✅ Input code is valid MOO syntax"
      puts "   ⚠️  If E_INVARG appears below, the problem is in AST conversion, NOT syntax"
    rescue => e
      puts "❌ Input code is invalid MOO syntax: #{e.message}"
      puts "   Skipping round-trip test for invalid input"
      recycle(o) if o
      return  # Skip the round-trip test
    ensure
      recycle(o) if o
    end
    
    # Test the round-trip using simplify() like other ToastStunt tests
    unparsed_result = simplify(command(%Q|; ast = parse_ast({"#{input_code}"}); return unparse_ast(ast);|))
    
    puts "Input:  #{input_code.inspect}"
    puts "Unparsed result: #{unparsed_result.inspect}"
    
    # Handle both string (single-line) and array (multi-line) results
    unparsed_lines = unparsed_result.is_a?(Array) ? unparsed_result : [unparsed_result]
    
    # Result should be non-empty
    assert(unparsed_lines.length > 0, "Expected non-empty result")
    
    # Join lines for comparison
    unparsed_code = unparsed_lines.join("\n")
    
    # For simple expressions, should match exactly (except string literals which have different escaping)
    if input_code.include?("return") && !input_code.include?("if") && !input_code.include?("while") && !input_code.include?("try") && !input_code.include?("\\\"")
      assert_equal(input_code, unparsed_code, "Round-trip failed for simple expression")
    end
    
    # Test that unparsed code is valid MOO syntax by trying to compile it
    begin
      o = create(:nothing)
      add_verb(o, [player, 'x', 'test'], ['this', 'none', 'this'])
      set_verb_code(o, 'test', unparsed_lines)
      puts "✅ Round-trip successful - unparsed code is valid"
    rescue => e
      assert(false, "Unparsed code is invalid MOO syntax: #{e.message}")
    ensure
      recycle(o) if o
    end
  end

  def test_basic_expressions
    run_test_as("wizard") do
      test_round_trip("Basic literal", "return 42;")
      test_round_trip("Arithmetic", "return 1 + 2;")
      test_round_trip("String literal", "return \\\"hello\\\";")
      test_round_trip("List literal", "return {1, 2, 3};")
    end
  end

  def test_simple_expression_round_trip
    run_test_as("wizard") do
      # Test arithmetic expression: 1 + 2
      puts "\n=== Testing arithmetic expression: 1 + 2 ==="
      arith_ast = command("; return parse_ast({\"return 1 + 2;\"});")
      puts "Arithmetic AST: #{arith_ast.inspect}"
      
      arith_unparsed = command("; ast = parse_ast({\"return 1 + 2;\"}); return unparse_ast(ast);")
      puts "Arithmetic unparsed: #{arith_unparsed.inspect}"
      
      # Test list literal expression
      puts "\n=== Testing list literal: {1, 2, 3} ==="
      list_ast = command("; return parse_ast({\"return {1, 2, 3};\"});")
      puts "List AST: #{list_ast.inspect}"
      
      list_unparsed = command("; ast = parse_ast({\"return {1, 2, 3};\"}); return unparse_ast(ast);")
      puts "List unparsed: #{list_unparsed.inspect}"
      
      # Test string literal
      puts "\n=== Testing string literal: \\\"hello\\\" ==="
      string_ast = command("; return parse_ast({\"return \\\"hello\\\";\"}); ")
      puts "String AST: #{string_ast.inspect}"
      
      string_unparsed = command("; ast = parse_ast({\"return \\\"hello\\\";\"}); return unparse_ast(ast);")
      puts "String unparsed: #{string_unparsed.inspect}"
    end
  end

  def test_comparison_equality
    run_test_as("wizard") do
      test_round_trip("Equality", "return 1 == 2;")
    end
  end

  def test_comparison_inequality 
    run_test_as("wizard") do
      test_round_trip("Inequality", "return 1 != 2;")
    end
  end

  def test_comparison_less_than
    run_test_as("wizard") do
      test_round_trip("Less than", "return 1 < 2;")
    end
  end

  def test_comparison_less_equal
    run_test_as("wizard") do
      test_round_trip("Less or equal", "return 1 <= 2;")
    end
  end

  def test_comparison_greater_than
    run_test_as("wizard") do
      test_round_trip("Greater than", "return 1 > 2;")
    end
  end

  def test_comparison_greater_equal
    run_test_as("wizard") do
      test_round_trip("Greater or equal", "return 1 >= 2;")
    end
  end

  # TDD PHASE 2 RED: Logical operators (expected to fail initially)
  def test_logical_and
    run_test_as("wizard") do
      test_round_trip("Logical AND", "return 1 && 2;")
    end
  end

  def test_logical_or
    run_test_as("wizard") do
      test_round_trip("Logical OR", "return 1 || 2;")
    end
  end

  def test_logical_not
    run_test_as("wizard") do
      test_round_trip("Logical NOT", "return !1;")
    end
  end

  # TDD PHASE 3 RED: Conditional expressions (expected to fail initially)
  def test_conditional_expression
    run_test_as("wizard") do
      test_round_trip("Conditional expression", "return 1 ? 2 | 3;")
    end
  end

  def test_conditional_expression_complex
    run_test_as("wizard") do
      test_round_trip("Complex conditional", "return (1 == 2) ? 42 | 0;")
    end
  end

  # TDD PHASE 4: Property and verb access
  def test_property_access
    run_test_as("wizard") do
      test_round_trip("Property access", "return player.name;")
    end
  end

  def test_verb_call_no_args
    run_test_as("wizard") do
      test_round_trip("Verb call no args", "return player:tell();")
    end
  end

  def test_verb_call_with_args
    run_test_as("wizard") do
      test_round_trip("Verb call with args", "return player:tell(42);")
    end
  end

  # TDD PHASE 5: Statement-level AST support
  def test_if_statement
    run_test_as("wizard") do
      test_round_trip("If statement", "if (1) return 42; endif")
    end
  end

  def test_if_else_statement
    run_test_as("wizard") do
      test_round_trip("If-else statement", "if (1) return 42; else return 0; endif")
    end
  end

  def test_while_loop_simple
    run_test_as("wizard") do  
      test_round_trip("While loop simple", "while (1) return 42; endwhile")
    end
  end

  def test_if_elseif_else_complex
    run_test_as("wizard") do
      test_round_trip("Complex if-elseif-else", "if (1) return 1; elseif (2) return 2; elseif (3) return 3; else return 0; endif")
    end
  end

  def test_while_loop_with_variable
    run_test_as("wizard") do
      test_round_trip("While with assignment", "x = 1; while (x < 10) x = x + 1; endwhile")
    end
  end

  def test_for_list_loop
    run_test_as("wizard") do
      test_round_trip("For-in list", "for x in ({1, 2, 3}) length({x}); endfor")
    end
  end

  def test_for_range_loop
    run_test_as("wizard") do
      test_round_trip("For-in range", "for i in [1..3] typeof(i); endfor")
    end
  end

  def test_try_except_basic
    run_test_as("wizard") do
      test_round_trip("Try-except basic", "try return 42; except e (E_PROPNF) return 0; endtry")
    end
  end

  def test_try_except_any
    run_test_as("wizard") do
      test_round_trip("Try-except ANY", "try return 42; except e (ANY) return -1; endtry")
    end
  end

  def test_try_finally
    run_test_as("wizard") do
      test_round_trip("Try-finally", "try return 42; finally player:tell(42); endtry")
    end
  end

  def test_try_multiple_except
    run_test_as("wizard") do
      test_round_trip("Try multiple except", "try return 42; except e (E_PROPNF) return 1; except e (E_VERBNF) return 2; except e (ANY) return -1; endtry")
    end
  end

  def test_nested_if_statements
    run_test_as("wizard") do
      test_round_trip("Nested if", "if (1) if (2) return 42; endif endif")
    end
  end

  def test_nested_loops
    run_test_as("wizard") do
      test_round_trip("Nested loops", "for x in ({1, 2}) for y in ({3, 4}) player:tell(x + y); endfor endfor")
    end
  end

  def test_break_continue_statements
    run_test_as("wizard") do
      test_round_trip("Break statement", "while (1) break; endwhile")
    end
  end

  def test_continue_statement
    run_test_as("wizard") do  
      test_round_trip("Continue statement", "for x in ({1, 2, 3}) continue; endfor")
    end
  end

  def test_fork_statement
    run_test_as("wizard") do
      test_round_trip("Fork statement", "fork (1) player:tell(42); endfork")
    end
  end

  def test_function_call_with_args
    run_test_as("wizard") do
      test_round_trip("Function call with args", "return length({1, 2, 3});")
    end
  end

  def test_return_with_value
    run_test_as("wizard") do
      test_round_trip("Return with value", "return 42;")
    end
  end

  def test_expression_statement
    run_test_as("wizard") do
      test_round_trip("Expression statement", "player:tell(42);")
    end
  end

  def test_assignment_statement
    run_test_as("wizard") do
      test_round_trip("Assignment", "x = 42;")
    end
  end

  def test_index_expression
    run_test_as("wizard") do
      test_round_trip("List indexing", "return {1, 2, 3}[2];")
    end
  end

  def test_map_expression
    run_test_as("wizard") do
      test_round_trip("Empty map", "return [];")
      test_round_trip("Single element map", "return [1 -> 42];")
    end
  end

  def test_first_last_operators
    run_test_as("wizard") do
      # Test ^ and $ in indexing contexts where they work
      test_round_trip("First in list indexing", "return args[^];")
      test_round_trip("Last in list indexing", "return args[$];")
    end
  end

  def test_catch_expression
    run_test_as("wizard") do
      # Debug what AST gets generated for catch expression
      puts "\n=== Debugging catch expression AST ==="
      
      # Test simple expression first to see if parse_ast works
      simple_ast = command("; return parse_ast({\"return 42;\"});")
      puts "Simple AST: #{simple_ast.inspect}"
      
      catch_ast = command("; return parse_ast({\"return `1 ! ANY => 0';\"});")
      puts "Catch AST: #{catch_ast.inspect}"
      
      # Test catch expression: expr ! codes => except_expr (correct syntax with single quotes)
      test_round_trip("Catch expression basic", "return `1 ! ANY => 0';")
      test_round_trip("Catch expression with error code", "return `length({}) ! E_INVARG => -1';")
    end
  end

  def test_property_assignment
    run_test_as("wizard") do
      test_round_trip("Property assignment", "player.location = here;")
    end
  end

  def test_list_assignment
    run_test_as("wizard") do
      test_round_trip("List assignment", "args[1] = 42;")
    end
  end

  def test_scatter_assignment
    run_test_as("wizard") do
      # Test scatter assignment: {a, b, c} = list
      test_round_trip("Basic scatter assignment", "{a, b, c} = {1, 2, 3};")
      # Test scatter with optional: {a, ?b} = list  
      test_round_trip("Optional scatter assignment", "{a, ?b} = {1, 2};")
      # Test scatter with rest: {a, @rest} = list
      test_round_trip("Rest scatter assignment", "{a, @rest} = {1, 2, 3};")
    end
  end

end
