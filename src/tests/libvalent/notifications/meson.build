# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2022 Andy Holmes <andrew.g.r.holmes@gmail.com>

# Dependencies
libvalent_notifications_test_deps = [
  libvalent_core_dep,
  libvalent_notifications_dep,
  libvalent_test_dep,
]

libvalent_notifications_tests = [
  'test-notifications-component',
]

foreach test : libvalent_notifications_tests
  test_program = executable(test, '@0@.c'.format(test),
                 c_args: test_c_args,
           dependencies: libvalent_notifications_test_deps,
    include_directories: libvalent_include_directories,
              link_args: test_link_args,
             link_whole: [libvalent_test, libvalent_notifications],
  )

  test(test, test_program,
            env: tests_env,
    is_parallel: false,
          suite: ['components', 'notifications'],
  )
endforeach
