#include <cstring>
#include <stdio.h>
#include <tree_sitter/api.h>

extern "C" TSLanguage *tree_sitter_cpp();

/**
 * \brief 
 * \return 
 */
int main() {
    // 创建一个解析器
    TSParser *parser = ts_parser_new();

    // 设置解析器的语言
    ts_parser_set_language(parser, tree_sitter_cpp());

    // 解析源代码
    const char *source_code = "void foo() {}";
    TSTree *tree = ts_parser_parse_string(parser, NULL, source_code, strlen(source_code));

    // 获取抽象语法树的根节点
    TSNode root_node = ts_tree_root_node(tree);

    // 遍历抽象语法树
    TSNode child_node = ts_node_named_child(root_node, 0);
    while (ts_node_is_null(child_node) == false) {
        // 打印节点的类型
        printf("Node type: %s\n", ts_node_type(child_node));

        // 如果节点是一个函数定义，打印函数名
        if (strcmp(ts_node_type(child_node), "function_definition") == 0) {
            TSNode function_name_node = ts_node_child_by_field_name(child_node, "declarator", strlen("declarator"));
            if (ts_node_is_null(function_name_node) == false) {
                uint32_t start_byte = ts_node_start_byte(function_name_node);
				uint32_t end_byte = ts_node_end_byte(function_name_node);
				printf("Function name: %.*s\n", end_byte - start_byte, &source_code[start_byte]);
            }
        }

        // 获取下一个节点
        child_node = ts_node_next_named_sibling(child_node);
    }

    // 删除解析器和抽象语法树
    ts_tree_delete(tree);
    ts_parser_delete(parser);

    return 0;
}