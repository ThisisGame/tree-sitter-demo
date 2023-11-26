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

#define InsertTraceToFunction 1

#define WriteInsertTrace 1

#define ParseFunction 1

// 输出日志并且写入到log文件
#define PRINT_MSG(...) \
    std::cout << __VA_ARGS__ << std::endl; \
	log_file << __VA_ARGS__ << std::endl;

// 输出绿色颜色的日志并且写入到log文件
#define PRINT_MSG_GREEN(...) \
    std::cout << "\033[1;32m" << __VA_ARGS__ << "\033[0m" << std::endl; \
    log_file << __VA_ARGS__ << std::endl;

// 输出红色颜色的日志并且写入到log文件
#define PRINT_MSG_RED(...) \
    std::cout << "\033[1;31m" << __VA_ARGS__ << "\033[0m" << std::endl; \
    log_file << __VA_ARGS__ << std::endl;

// 切换到下一个node
#define NODE_CONTINUE() \
                                                            node = ts_node_next_named_sibling(node); \
                                                            continue;

// 输出红色错误日志，并且切换到下一个node
#define NODE_ERROR_CONTINUE(NodeName,NodeCode) \
															log_file << NodeName << " has error--->\n" << NodeCode << std::endl; \
															std::cout<<"\033[1;31m"<<NodeName<<" has error--->\033[0m\n"<<NodeCode<<std::endl; \
															node = ts_node_next_named_sibling(node); \
															continue;

// 输出红色错误日志，并且如果节点有子节点，递归遍历
#define NODE_ERROR_CONTINUE_TRAVERSE(NodeName,NodeCode) \
                                                            log_file << NodeName << " has error--->\n" << NodeCode << std::endl; \
                                                            std::cout<<"\033[1;31m"<<NodeName<<" has error--->\033[0m\n"<<NodeCode<<std::endl; \
                                                            if (ts_node_child_count(node) > 0) { \
                                                                traverse_and_print(ts_node_named_child(node, 0), source_code, insertions,log_file); \
                                                            } \
                                                            node = ts_node_next_named_sibling(node); \
                                                            continue;

// 输出绿色日志，并且切换到下一个node
#define NODE_PRINT_CONTINUE(NodeName,NodeCode) \
                                                            log_file << NodeName << " has error--->\n" << NodeCode << std::endl; \
                                                            std::cout<<"\033[1;32m"<<NodeName<<" has error--->\033[0m\n"<<NodeCode<<std::endl; \
                                                            node = ts_node_next_named_sibling(node); \
                                                            continue;

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
void backup_files(const std::vector<std::string>& files, const std::string& source_directory, const std::string& backup_directory) {
    std::filesystem::create_directories(backup_directory);
    for (const auto& file : files) {
        // 获取文件相对于源目录的路径
        std::filesystem::path relative_path = std::filesystem::relative(file, source_directory);

        // 在备份目录中创建相同的路径
        std::filesystem::path backup_path = backup_directory / relative_path;

        // 创建备份文件的目录
        std::filesystem::create_directories(backup_path.parent_path());

        // 复制文件
        std::filesystem::copy(file, backup_path, std::filesystem::copy_options::overwrite_existing);
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

TSNode ts_find_node_in_first_child_level_by_type(TSNode node, const char* node_type) {
    // 遍历所有子节点
    uint32_t child_count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child_node = ts_node_named_child(node, i);
        if (ts_node_is_null(child_node) == 1) {
            continue;
        }

        // 获取当前节点的类型
	    const char* child_node_type = ts_node_type(child_node);

	    // 检查当前节点的类型是否是指定的类型
	    if (strcmp(child_node_type, node_type) == 0) {
	        return child_node;
	    }
    }

    // 如果没有找到指定类型的节点，返回一个空节点
    return TSNode();
}

bool ts_check_node_source_code(const std::string& source_code,TSNode node, const char* in_node_code) {
    if(ts_node_is_null(node)==false)
    {
        std::string node_code = source_code.substr(ts_node_start_byte(node), ts_node_end_byte(node) - ts_node_start_byte(node));
        if (in_node_code==node_code) {
	        return true;
	    }
    }
    return false;
}

TSNode ts_find_error_node(TSNode node) {
    const char* node_type = ts_node_type(node);
    if (strcmp(node_type, "ERROR") == 0) {
        return node;
    }

    uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; i++) {
        TSNode child = ts_node_child(node, i);
        TSNode error_node = ts_find_error_node(child);
        if (ts_node_is_null(error_node)==false && strcmp(ts_node_type(error_node), "ERROR") == 0) {
            return error_node;
        }
    }

    // Return an empty node if no error node was found
    return TSNode();
}


/**
 * \brief 遍历并打印节点及其所有子节点
 * \param node 要遍历的节点
 * \param source_code 源代码字符串
 * \param insertions 保存需要插入的字符串和位置的向量
 */
void traverse_and_print(TSNode node, const std::string& source_code, std::vector<std::pair<size_t, std::string>>& insertions,std::ofstream& log_file) {
    while (ts_node_is_null(node) == false) {
        // 打印节点的类型
        const char* node_type = ts_node_type(node);
        //PRINT_MSG("Node type: "<<node_type)

    	std::string node_code = source_code.substr(ts_node_start_byte(node), ts_node_end_byte(node) - ts_node_start_byte(node));
        //PRINT_MSG("Node code: "<<node_code)

        // function_definition节点下第一层子节点存在function_declarator
        // 在function_declarator第一层子节点查找函数定义(静态函数identifier/field_identifier/qualified_identifier)和参数(parameter_list)

#if ParseFunction
        // 如果节点是函数，添加TRACE_CPUPROFILER_EVENT_SCOPE
        if (strcmp(node_type, "function_definition") == 0) 
        {
            // function_definition第一层子级存在type_qualifier类型，且内容等于constexpr，这种不能在里面插入Trace宏
            TSNode constexpr_node=ts_find_node_in_first_child_level_by_type(node,"type_qualifier");
            if(ts_check_node_source_code(source_code,constexpr_node,"constexpr"))
            {
				NODE_ERROR_CONTINUE("constexpr can't trace",node_code)
            }

            // 在第一层查找function_declarator
            TSNode function_declarator_node = ts_find_node_in_first_child_level_by_type(node, "function_declarator");
            if(ts_node_is_null(function_declarator_node))
            {
                NODE_ERROR_CONTINUE_TRAVERSE("function_declarator",node_code)
            }
            std::string function_declarator_node_code = source_code.substr(ts_node_start_byte(function_declarator_node), ts_node_end_byte(function_declarator_node) - ts_node_start_byte(function_declarator_node));

            // 在function_declarator第一层子节点查找函数定义(静态函数identifier/field_identifier/qualified_identifier)和参数(parameter_list)
            TSNode function_declarator_identifier_node = ts_find_node_in_first_child_level_by_type(function_declarator_node, "identifier");
            TSNode function_declarator_field_identifier_node = ts_find_node_in_first_child_level_by_type(function_declarator_node, "field_identifier");
            TSNode function_declarator_qualified_identifier_node = ts_find_node_in_first_child_level_by_type(function_declarator_node, "qualified_identifier");
            TSNode function_declarator_parameter_list_node = ts_find_node_in_first_child_level_by_type(function_declarator_node, "parameter_list");

            // 验证不通过，没有函数定义
            if(ts_node_is_null(function_declarator_identifier_node) 
                && ts_node_is_null(function_declarator_field_identifier_node)
                && ts_node_is_null(function_declarator_qualified_identifier_node))
            {
	            NODE_ERROR_CONTINUE_TRAVERSE("identifier",node_code)
            }

            // 验证不通过，没有参数列表
            if(ts_node_is_null(function_declarator_parameter_list_node))
            {
                NODE_ERROR_CONTINUE_TRAVERSE("parameter_list",node_code)
            }

            //获取函数体compound_statement
            TSNode compound_statement_node = ts_node_child_by_node_type(node, "compound_statement");
        	if(ts_node_is_null(compound_statement_node))
            {
                NODE_ERROR_CONTINUE("compound_statement",node_code)
            }
            std::string compound_statement_node_code = source_code.substr(ts_node_start_byte(compound_statement_node), ts_node_end_byte(compound_statement_node) - ts_node_start_byte(compound_statement_node));

            //获取函数体的第一个child node，在它插入代码
            if (ts_node_child_count(compound_statement_node) > 1) {
                TSNode first_child_node = ts_node_child(compound_statement_node, 1);
                if(ts_node_is_null(first_child_node))
	            {
	                NODE_ERROR_CONTINUE("first_child_node",node_code)
	            }
                std::string first_child_node_code = source_code.substr(ts_node_start_byte(first_child_node), ts_node_end_byte(first_child_node) - ts_node_start_byte(first_child_node));

                // 获取开始位置
				uint32_t first_child_start = ts_node_start_byte(first_child_node);

                // 获取函数名
	            TSNode function_name_node = ts_node_child_by_field_name(function_declarator_node, "declarator", strlen("declarator"));
	            if(ts_node_is_null(function_name_node))
	            {
	                NODE_ERROR_CONTINUE("function_name_node",node_code)
	            }
	            std::string function_name = source_code.substr(ts_node_start_byte(function_name_node), ts_node_end_byte(function_name_node) - ts_node_start_byte(function_name_node));

                // 判断函数名是否异常(是否有多行)
				if(function_name.find('\n') != std::string::npos)
				{
					NODE_ERROR_CONTINUE("function_name multiline",function_name)
				}

                std::string trace_line = "TRACE_CPUPROFILER_EVENT_SCOPE(" + function_name + ");";

                // 判断是否已经插入过TRACE_CPUPROFILER_EVENT_SCOPE
                if (first_child_node_code.find("TRACE_CPUPROFILER_EVENT_SCOPE") != std::string::npos) {
                    NODE_CONTINUE()
                }

                PRINT_MSG_GREEN("function_name: "<<function_name)

#if InsertTraceToFunction
                // 获取函数体与第一个Node之间的空白字符
                std::string blank_chars = source_code.substr(ts_node_start_byte(compound_statement_node)+1, ts_node_start_byte(first_child_node) - ts_node_start_byte(compound_statement_node)-1);

                trace_line += blank_chars;
	            insertions.push_back({first_child_start, trace_line});
#endif
            }
        }
#endif

        // 如果节点有子节点，递归遍历
        if (ts_node_child_count(node) > 0) {
            traverse_and_print(ts_node_named_child(node, 0), source_code, insertions,log_file);
        }

        // 获取下一个节点
        node = ts_node_next_named_sibling(node);
    }
}



int main(int argc, char* argv[]) {
    std::time_t t = std::time(nullptr);
	char buf[100];
	std::strftime(buf, sizeof(buf), "%Y-%m-%d-%H-%M-%S", std::localtime(&t));
	std::string filename = std::string("log-") + buf;
	std::ofstream log_file(filename, std::ios_base::app);

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

#if InsertTraceToFunction
    // 获取目录名
    std::string dir_name = std::filesystem::path(argv[1]).filename().string();

    // 备份 .cpp 文件
    backup_files(cpp_files, argv[1],"./" + dir_name + "_bak_" + ss.str());
#endif

    // 创建一个解析器
    TSParser *parser = ts_parser_new();

    // 设置解析器的语言
    ts_parser_set_language(parser, tree_sitter_cpp());

    // 遍历并处理所有的 .cpp 文件
    for (const auto& file_path : cpp_files) {
        PRINT_MSG(file_path)

        // 解析源代码
        std::ifstream file(file_path);
        if (!file) {
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
        traverse_and_print(root_node, source_code, insertions,log_file);

#if WriteInsertTrace
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
#endif

        // 删除抽象语法树
        ts_tree_delete(tree);
    }

    // 删除解析器
    ts_parser_delete(parser);

    return 0;
}