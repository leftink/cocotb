# A set of regression tests for open issues

import cocotb
import logging
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer
from cocotb.result import TestFailure

@cocotb.test()
def issue_813(dut):
    tlog = logging.getLogger("cocotb.test")

    cocotb.fork(Clock(dut.clk, 2500).start())

    if dut.counters.COUNTER_LEN != 8:
        tlog.error("COUNTER_LEN is %d, but expected 8", int(dut.counters.COUNTER_LEN))

    dut.rst <= 1
    dut.enable <= 0
    yield Timer(10000)
    dut.rst <= 0
    yield Timer(10000)
    dut.enable <= 1

    while dut.done != 15:
        yield RisingEdge(dut.clk)
