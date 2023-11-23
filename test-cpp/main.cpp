#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdio.h>
#include <tree_sitter/api.h>
#include <vector>

extern "C" TSLanguage *tree_sitter_cpp();

/**
 * \brief 遍历并打印节点及其所有子节点
 * \param node 要遍历的节点
 * \param source_code 源代码字符串
 * \param insertions 保存需要插入的字符串和位置的向量
 */
void traverse_and_print(TSNode node, const std::string& source_code, std::vector<std::pair<size_t, std::string>>& insertions) {
    while (ts_node_is_null(node) == false) {
        // 打印节点的类型
        const char* node_type = ts_node_type(node);
        printf("Node type: %s\n", node_type);

        // 打印节点内容
        uint32_t start_byte = ts_node_start_byte(node);
        uint32_t end_byte = ts_node_end_byte(node);
        printf("Node content: %.*s\n", end_byte - start_byte, &source_code[start_byte]);

        // 如果节点是函数，添加TRACE_CPUPROFILER_EVENT_SCOPE
        if (strcmp(node_type, "function_declarator") == 0) {
            // 获取函数名
            TSNode function_name_node = ts_node_child_by_field_name(node, "identifier", strlen("identifier"));
            std::string function_name = source_code.substr(ts_node_start_byte(function_name_node), ts_node_end_byte(function_name_node) - ts_node_start_byte(function_name_node));

            // 获取函数体
            TSNode function_body_node = ts_node_child_by_field_name(node, "body", strlen("body"));
            uint32_t function_body_start = ts_node_start_byte(function_body_node);

            // 找到"{"后的第一个换行符
            size_t insert_pos = source_code.find('\n', function_body_start);
            if (insert_pos != std::string::npos) {
                // 记录需要插入的字符串和位置
                std::string trace_line = "TRACE_CPUPROFILER_EVENT_SCOPE(" + function_name + ");\n";
                insertions.push_back({insert_pos + 1, trace_line});
            }
        }

        // 如果节点有子节点，递归遍历
        if (ts_node_child_count(node) > 0) {
            traverse_and_print(ts_node_named_child(node, 0), source_code, insertions);
        }

        // 获取下一个节点
        node = ts_node_next_named_sibling(node);
    }
}

int main() {
    // 创建一个解析器
    TSParser *parser = ts_parser_new();

    // 设置解析器的语言
    ts_parser_set_language(parser, tree_sitter_cpp());

    // 解析源代码
    std::ifstream file("data/SCompoundWidget.cpp");
    if (!file) {
        std::cout << "Failed to open file\n";
        return 1;
    }
    std::string source_code((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    file.close();
    TSTree *tree = ts_parser_parse_string(parser, NULL, source_code.c_str(), source_code.size());

    // 获取抽象语法树的根节点
    TSNode root_node = ts_tree_root_node(tree);

    // 遍历抽象语法树并记录需要插入的字符串和位置
    std::vector<std::pair<size_t, std::string>> insertions;
    traverse_and_print(root_node, source_code, insertions);

    // 按照位置从大到小的顺序插入字符串，这样不会影响到其他插入位置的正确性
    std::sort(insertions.begin(), insertions.end(), [](const std::pair<size_t, std::string>& a, const std::pair<size_t, std::string>& b) {
        return a.first > b.first;
    });

	for (const auto& insertion : insertions) {
        source_code.insert(insertion.first, insertion.second);
    }

    // 保存修改后的代码
    std::ofstream out_file("data/SCompoundWidget_modified.cpp");
    out_file << source_code;
    out_file.close();

    // 删除解析器和抽象语法树
    ts_tree_delete(tree);
    ts_parser_delete(parser);

    return 0;
}