basic_world_exe = executable('basic world', 'basic_world.c',
  dependencies : ciggurat_dep)
world_user_data_exe = executable('world user data', 'world_user_data.c',
  dependencies : ciggurat_dep)

test('basic world', basic_world_exe, suite : 'world')
test('world user data', world_user_data_exe, suite : 'world')
