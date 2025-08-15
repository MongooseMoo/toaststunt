require 'test_helper'

class TestAst < Test::Unit::TestCase

  def test_that_parse_ast_function_exists
    run_test_as('wizard') do
      result = command("; parse_ast({\"return 42;\"});")
      assert_not_equal E_VERBNF, result
    end
  end

  def test_that_parse_ast_requires_wizard_permissions
    run_test_as('programmer') do
      result = command("; parse_ast({\"return 42;\"});")
      assert_equal E_PERM, simplify(result)
    end
  end

  def test_that_parse_ast_validates_arguments
    run_test_as('wizard') do
      # Test wrong number of arguments
      assert_equal E_ARGS, simplify(command("; parse_ast();"))
      assert_equal E_ARGS, simplify(command("; parse_ast({\"test\"}, {\"extra\"});"))
      
      # Test wrong argument type
      assert_equal E_TYPE, simplify(command("; parse_ast(\"not a list\");"))
      assert_equal E_TYPE, simplify(command("; parse_ast(42);"))
    end
  end

  def test_that_validate_ast_function_exists
    run_test_as('wizard') do
      result = command("; validate_ast([\"foo\" -> \"bar\"]);")
      assert_not_equal E_VERBNF, result
    end
  end

  def test_that_validate_ast_validates_arguments
    run_test_as('wizard') do
      # Test wrong number of arguments
      assert_equal E_ARGS, simplify(command("; validate_ast();"))
      assert_equal E_ARGS, simplify(command("; validate_ast({}, {});"))
      
      # Test wrong argument type
      assert_equal E_TYPE, simplify(command("; validate_ast(\"not a map\");"))
      assert_equal E_TYPE, simplify(command("; validate_ast({1, 2, 3});"))
    end
  end

  def test_that_unparse_ast_function_exists
    run_test_as('wizard') do
      result = command("; unparse_ast([\"type\" -> \"placeholder\"]);")
      assert_not_equal E_VERBNF, result
    end
  end

  def test_basic_parse_ast_functionality
    run_test_as('wizard') do
      # Test basic parsing returns a MAP with correct version
      result = command("; parse_ast({\"return 42;\"});")
      # Should return a MAP with ast_version and type fields
      # For now just verify it's not an error
      assert_not_equal E_INVARG, simplify(result)
      assert_not_equal E_TYPE, simplify(result)
    end
  end

  def test_enhanced_parse_ast_output
    run_test_as('wizard') do
      # Test what the AST implementation returns
      result = command("; ast = parse_ast({\"x = 1;\"}); return {ast[\"type\"], ast[\"ast_version\"]};")
      puts "AST structure test:"
      puts "Result: #{result.inspect}"
      
      # Just verify it returns something reasonable
      assert_not_equal E_INVARG, simplify(result)
      
      # Extract the AST and examine its structure
      ast_result = command("; return parse_ast({\"x = 1;\"});")
      puts "Full AST result: #{ast_result.inspect}"
      
      # Test another simple statement with valid builtin function
      ast_call = command("; return parse_ast({\"length({1, 2, 3});\"});")
      puts "Function call AST: #{ast_call.inspect}"
    end
  end

end