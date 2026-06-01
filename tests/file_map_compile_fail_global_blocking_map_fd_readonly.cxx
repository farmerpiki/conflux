import conflux.file_map;

int main() {
    auto fn = &blocking_map_fd_readonly;
    return fn == nullptr ? 0 : 1;
}
