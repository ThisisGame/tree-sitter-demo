#include <filesystem>
#include <iostream>


// ����Ŀ¼���ҵ����е� .cpp �ļ�
std::vector<std::string> find_cpp_files(const std::string& directory) {
    std::vector<std::string> cpp_files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (entry.path().extension() == ".cpp") {
            cpp_files.push_back(entry.path().string());
        }
    }
    return cpp_files;
}

// �����ļ�
void backup_files(const std::vector<std::string>& files, const std::string& source_directory, const std::string& backup_directory) {
    std::filesystem::create_directories(backup_directory);
    for (const auto& file : files) {
        // ��ȡ�ļ������ԴĿ¼��·��
        std::filesystem::path relative_path = std::filesystem::relative(file, source_directory);

        // �ڱ���Ŀ¼�д�����ͬ��·��
        std::filesystem::path backup_path = backup_directory / relative_path;

        // ���������ļ���Ŀ¼
        std::filesystem::create_directories(backup_path.parent_path());

        // �����ļ�
        std::filesystem::copy(file, backup_path, std::filesystem::copy_options::overwrite_existing);

        // ���ñ����ļ�������޸�ʱ��Ϊ��ǰʱ��
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

    // �ҵ����е� .cpp �ļ�
    std::vector<std::string> cpp_files = find_cpp_files(argv[1]);

    // ���� .cpp �ļ�
    backup_files(cpp_files, argv[1],argv[2]);

    return 0;
}