add_executable(replay_cli main.cpp)
target_link_libraries(replay_cli PRIVATE ft_sources ft_storage ft_core)
ft_warnings(replay_cli)
