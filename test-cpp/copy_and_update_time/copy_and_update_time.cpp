#include <filesystem>
#include <iostream>


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

        // 设置备份文件的最后修改时间为当前时间
        std::filesystem::last_write_time(backup_path, std::filesystem::file_time_type::clock::now());
    }
}


int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cout << "Usage: program source_folder destination_folder\n";
        return 1;
    }

    std::filesystem::path src_folder = argv[1];
    std::filesystem::path dst_folder = argv[2];

    // 找到所有的 .cpp 文件
    std::vector<std::string> cpp_files = find_cpp_files(argv[1]);

    // 备份 .cpp 文件
    backup_files(cpp_files, argv[1],argv[2]);

    return 0;
}