# kind4 区域站点的 d_reg 必须等于“区域最终输出寄存器”，它不一定等于原 mul 的 Rd
# （expected_d_reg）。历史缺陷：EF5D8 的 d_reg 误填 22，thunk 写 w22，而 ef5f0 的物防
# 调用读取 w2，导致 w2 保留调用前垃圾值（真机出现 -593799923 / 六到七位暴击伤害）。
#
# 这里锁定六个 C18 Act_DD 站点的输出寄存器；新增/修改站点时必须同步本检查。
if(NOT DEFINED CPP_DIR)
    message(FATAL_ERROR "CPP_DIR is required")
endif()

file(READ "${CPP_DIR}/game/access/insn_sites.cpp" content)

set(expected
    "{\"INSN_ACT_M_DD_WRAP_A\", F_CHAR_ACT_M_DD_WRAP_A_VMA, 0, 23, 0, 2, 1000,"
    "{\"INSN_ACT_M_DD_WRAP_B\", F_CHAR_ACT_M_DD_WRAP_B_VMA, 1, 21, 2, 21, 0, 0}"
    "{\"INSN_ACT_M_DD_WRAP_BC\", F_CHAR_ACT_M_DD_WRAP_BC_VMA, 3, 21, 0, 2, 1000,"
    "{\"INSN_ACT_DD_EF20C\", 0xef20c, 4, 27, 22, 22, 1000, 0xef234,"
    "{\"INSN_ACT_DD_EF4D0\", 0xef4d0, 4, 27, 0, 0, 1000, 0xef4e8,"
    "{\"INSN_ACT_DD_EF530\", 0xef530, 4, 22, 2, 22, 1000, 0xef560,"
    "{\"INSN_ACT_DD_EF560\", 0xef560, 4, 22, 0, 2, 1000, 0xef574,"
    "{\"INSN_ACT_DD_EF5D8\", 0xef5d8, 4, 22, 1, 2, 1000, 0xef5f0,"
    "{\"INSN_ATTR_ATTACK_EB1B4\", 0xeb1b4, 4, 3, 1, 26, 100, 0xeb1d0,")

foreach(line IN LISTS expected)
    string(FIND "${content}" "${line}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "insn site output register drifted: missing ${line}")
    endif()
endforeach()
