#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <cstdio>
#include <tree_sitter/api.h>
#include <vector>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>

extern "C" TSLanguage *tree_sitter_cpp();

// 遍历目录并找到所有的 .cpp 文件
std::vector<std::string> find_cpp_files(const std::string& directory) {
    std::vector<std::string> cpp_files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (entry.path().extension() == ".cpp") {
            cpp_files.push_back(entry.path().string());
        }
    }
    return cpp_files;
}

// 备份文件
void backup_files(const std::vector<std::string>& files, const std::string& backup_directory) {
    std::filesystem::create_directories(backup_directory);
    for (const auto& file : files) {
        std::filesystem::copy(file, backup_directory + "/" + std::filesystem::path(file).filename().string(), std::filesystem::copy_options::overwrite_existing);
    }
}

TSNode ts_node_child_by_node_type(TSNode node,const char* in_node_type)
{
	for(int i=0;i<ts_node_child_count(node);i++)
    {
        TSNode child_node = ts_node_child(node, i);
        const char* node_type = ts_node_type(child_node);
        if (strcmp(node_type, in_node_type) == 0)
        {
	        return child_node;
        }
    }
    return TSNode();
}

TSNode ts_find_node_by_type(TSNode node, const char* node_type) {
    // 获取当前节点的类型
    const char* type = ts_node_type(node);

    // 检查当前节点的类型是否是指定的类型
    if (strcmp(type, node_type) == 0) {
        return node;
    }

    // 遍历所有子节点
    uint32_t child_count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child_node = ts_node_named_child(node, i);
        TSNode result = ts_find_node_by_type(child_node, node_type);

        // 如果找到了指定类型的节点，返回它
        if (ts_node_is_null(result) == 0) {
            return result;
        }
    }

    // 如果没有找到指定类型的节点，返回一个空节点
    return TSNode();
}

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
        //printf("Node type: %s\n", node_type);

         std::string function_code = source_code.substr(ts_node_start_byte(node), ts_node_end_byte(node) - ts_node_start_byte(node));

        // 如果节点是函数，添加TRACE_CPUPROFILER_EVENT_SCOPE
        if (strcmp(node_type, "function_definition") == 0) {

            // 获取函数定义
            TSNode function_declarator_node = ts_find_node_by_type(node, "function_declarator");
            if(ts_node_is_null(function_declarator_node))
            {
                std::cout<<"\033[1;31mfunction_declarator_node null--->\033[0m\n"<<function_code<<std::endl;
                node = ts_node_next_named_sibling(node);
                continue;
            }
            std::string function_declarator_node_code = source_code.substr(ts_node_start_byte(function_declarator_node), ts_node_end_byte(function_declarator_node) - ts_node_start_byte(function_declarator_node));

            // 获取函数名
            TSNode function_name_node = ts_node_child_by_field_name(function_declarator_node, "declarator", strlen("declarator"));
            if(ts_node_is_null(function_name_node))
            {
                std::cout<<"\033[1;31mfunction_name_node null--->\033[0m\n"<<function_code<<std::endl;
                node = ts_node_next_named_sibling(node);
                continue;
            }
            std::string function_name = source_code.substr(ts_node_start_byte(function_name_node), ts_node_end_byte(function_name_node) - ts_node_start_byte(function_name_node));

            //获取函数体compound_statement
            TSNode compound_statement_node = ts_node_child_by_node_type(node, "compound_statement");
        	if(ts_node_is_null(compound_statement_node))
            {
                std::cout<<"\033[1;31mcompound_statement_node null--->\033[0m\n"<<function_code<<std::endl;
                node = ts_node_next_named_sibling(node);
                continue;
            }
            std::string compound_statement_node_code = source_code.substr(ts_node_start_byte(compound_statement_node), ts_node_end_byte(compound_statement_node) - ts_node_start_byte(compound_statement_node));

            //获取函数体的第一个child node
            if (ts_node_child_count(compound_statement_node) > 1) {
                TSNode first_child_node = ts_node_child(compound_statement_node, 1);
                if(ts_node_is_null(first_child_node))
	            {
                    std::cout<<"\033[1;31mfirst_child_node null--->\033[0m\n"<<function_code<<std::endl;
	                node = ts_node_next_named_sibling(node);
	                continue;
	            }
                std::string first_child_node_code = source_code.substr(ts_node_start_byte(first_child_node), ts_node_end_byte(first_child_node) - ts_node_start_byte(first_child_node));

                // 获取开始位置
				uint32_t first_child_start = ts_node_start_byte(first_child_node);
	            std::string trace_line = "TRACE_CPUPROFILER_EVENT_SCOPE(" + function_name + ");\n\t";
	            insertions.push_back({first_child_start, trace_line});
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

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cout << "Usage: " << argv[0] << " <directory>\n";
        return 1;
    }

    // 获取当前日期
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_c), "%Y-%m-%d_%H-%M-%S");

    // 找到所有的 .cpp 文件
    std::vector<std::string> cpp_files = find_cpp_files(argv[1]);

    // 获取目录名
    std::string dir_name = std::filesystem::path(argv[1]).filename().string();

    // 备份 .cpp 文件
    backup_files(cpp_files, std::filesystem::path(argv[1]).parent_path().string() + "/" + dir_name + "_bak_" + ss.str());

    // 创建一个解析器
    TSParser *parser = ts_parser_new();

    // 设置解析器的语言
    ts_parser_set_language(parser, tree_sitter_cpp());

    // 遍历并处理所有的 .cpp 文件
    for (const auto& file_path : cpp_files) {
        std::cout<<file_path<<std::endl;
        // 解析源代码
        std::ifstream file(file_path);
        if (!file) {
            std::cout << "Failed to open file: " << file_path << "\n";
            continue;
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

        // 覆盖原始文件
        std::ofstream out_file(file_path);
        out_file << source_code;
        out_file.close();

        // 删除抽象语法树
        ts_tree_delete(tree);
    }

    // 删除解析器
    ts_parser_delete(parser);

    return 0;
}