//
// Created by oliverw on 19.02.21.
//

#include "RISCVCpu.h"
#include "csr.h"

Instruction RISCVCpu::fetch() {
    Instruction i;
    i._ui32 = memory->read32(pc); //get 32 bit bit instruction form memory
    return i;
}

iec RISCVCpu::run_instruction(Instruction i) {
    auto o = (Opcode)i.d.opcode;
    switch (o) {
        case LOAD:
            return  load(i.i);
        case STORE:
            return store(i.s);
        case LUI:
            return lui(i.u);
        case ALU_IMM:
            return alu_imm(i.i);
        case ALU:
            return alu(i.r);
        case AUIPC:
            return auipc(i.u);
        case BRANCH:
            return branch(i.b);
        case JAL:
            return jal(i.j);
        case JALR:
            return jalr(i.i);
        case CSR:
            return csr_instr(i.i);
        default:
            return iec::Illegal;
    }
}

void RISCVCpu::execute(Instruction i) {
    auto o = (Opcode)i.d.opcode;

    if(check_local_interrupt() >0 ) {
        return;
    }

    iec except = run_instruction(i);

    if (except!= iec::IEC_OK) {
        exception(except);
        return;
    }

    if ( !(o == JAL || o == JALR || o == BRANCH || (o == CSR && (i.i.imm == 0x302)) ) ) {
        pc+=4; //increment pc if not branch or jump
    }
    x[0] = 0; //r0 is always 0

}

void RISCVCpu::exception(InstructionExceptionCode code) {
    run_interrupt(true,code);
}


typedef uint32_t(*aluFunc)(uint32_t rs1, uint32_t rs2);
const aluFunc aluLookup[8] = { [](uint32_t x, uint32_t y){return  x+y;},                                 //0 ADD
                               [](uint32_t x, uint32_t y){return  x << (y&0x1F);},                       //1 SLL
                               [](uint32_t x, uint32_t y){return  (((int32_t)x<(int32_t)y) ? 1u: 0u);},  //2 SLT
                               [](uint32_t x, uint32_t y){return  ((x<y) ? 1u: 0u);},                    //3 SLTU
                               [](uint32_t x, uint32_t y){return  x^y;},                                 //4 XOR
                               [](uint32_t x, uint32_t y){return  x>>(y&0x1F);},                         //5 SRL
                               [](uint32_t x, uint32_t y){return  x|y;},                                 //6 OR
                               [](uint32_t x, uint32_t y){return  x&y;}};                                //7 AND

const aluFunc aluLookupAlt[8] = {[](uint32_t x, uint32_t y){return  x-y;},                               //0 SUB
                                 nullptr, nullptr, nullptr, nullptr,
                                 [](uint32_t x, uint32_t y){return  uint32_t ((int32_t)x>>(y&0x1F));},                       //5 SRA
                                 nullptr, nullptr};
iec RISCVCpu::alu(rType ri) {
    if(ri.func7 != 0) {
        x[ri.rd] = aluLookupAlt[ri.func3](x[ri.rs1], x[ri.rs2]);
    } else {
        x[ri.rd] = aluLookup[ri.func3](x[ri.rs1], x[ri.rs2]);
    }
    return iec::IEC_OK;
}

iec RISCVCpu::alu_imm(iType aii) {
    if(aii.func3 == 5 && (aii.imm & 0x400) > 0) {
        // SRAI alterative function
        x[aii.rd] = aluLookupAlt[aii.func3](x[aii.rs1], imm_i(aii.imm));
    } else {
        x[aii.rd] = aluLookup[aii.func3](x[aii.rs1], imm_i(aii.imm));
    }
    return iec::IEC_OK;
}

void RISCVCpu::resetCpu() {
    for (int i = 0; i < 32; ++i) {
        x[0] = 0;
    }

    for (unsigned int & i : csr) {
        i = 0;
    }
    pc = 0;
}

void RISCVCpu::single_step() {
    Instruction  i = fetch();
    execute(i);

    instuction_count = (uint64_t)csr[CSR_MCYCLEH] << 32 | csr[CSR_MCYCLE];
    instuction_count++;
    csr[CSR_MCYCLEH] = instuction_count >> 32;
    csr[CSR_MCYCLE] = instuction_count;
    csr[CSR_MINSTRET]++;
    if(csr[CSR_MINSTRET] == 0) {csr[CSR_MINSTRET]++; }

}

iec RISCVCpu::store(sType si) {
    Address adr = x[si.rs1] + imm_s(si.imm_0_4, si.imm_5_11);
    uint32_t value = x[si.rs2];
    if(adr < IO_MAX && !mmio.empty()) {
        return write_io(static_cast<F3_StoreOp>(si.func3), adr, value);
    }

    switch (si.func3) {
        case 0: //byte
            memory->write08(value&0xFF, adr);
            break;
        case 1: //half-word
            memory->write16(value&0xFFFF, adr);
            break;
        case 2: //word
            memory->write32(value, adr);
            break;
        default:
            //we should never be here
            return iec::Illegal;
    }
    return iec::IEC_OK;
}


iec RISCVCpu::load(iType li) {
    Address adr = x[li.rs1] + imm_i(li.imm);
    uint32_t temp = 0;
    iec excep = iec::IEC_OK;

    if(adr < IO_MAX&& !mmio.empty()) {

        temp =read_io(static_cast<F3_LoadOp>(li.func3), adr, excep);

    } else {

        switch (li.func3) {
            case 0: //byte
                temp = memory->read08(adr);
                temp |= (temp & 0x0080) ? 0xFFFFFF00 : 0x00000000;
                break;
            case 1: //half-word
                temp = memory->read16(adr);
                temp |= (temp & 0x8000) ? 0xFFFF0000 : 0x00000000;
                break;
            case 2: //word
                temp = memory->read32(adr);
                break;
            case 4: //unsigned byte
                temp = memory->read08(adr);
                break;
            case 5: //unsigned half-word
                temp = memory->read16(adr);
                break;
            default:
                return iec::Illegal;

        }
    }

    x[li.rd] = temp;
    return excep;
}

uint32_t RISCVCpu::read_io(F3_LoadOp op, uint32_t addr, iec& excep ) {
    for (auto &&i : mmio)
    {
        if( i->in_device(addr) ) {
            excep = iec::IEC_OK;
            return i->read(op,addr);
        }
    }
    return iec::L_AccessFault;
}

iec RISCVCpu::write_io(F3_StoreOp op, uint32_t addr, uint32_t value) {
    for (auto &&i : mmio)
    {
        if (i->in_device(addr)) {
            i->write(op,addr,value);
            return iec::IEC_OK;

        }
    }
    return iec::S_AccessFault;
}

iec RISCVCpu::lui(uType ui) {
    x[ui.rd] = imm_u(ui.imm);
    return iec::IEC_OK;
}

iec RISCVCpu::auipc(uType ui) {
    x[ui.rd] = imm_u(ui.imm) + pc;
    return iec::IEC_OK;
}

iec RISCVCpu::jal(jType ji) {
    x[ji.rd] = pc+4;
    uint32_t imm = imm_j(ji.imm);
    pc += imm;
    return iec::IEC_OK;
}

iec RISCVCpu::jalr(iType jii) {
    uint32_t temp_pc = pc+4;
    uint32_t imm = imm_i(jii.imm);
    pc = (x[jii.rs1] + imm) & ~0x01;
    x[jii.rd] = temp_pc;
    return iec::IEC_OK;
}

iec RISCVCpu::branch(bType bi) {
    switch (bi.func3) {
        case 0:    //beq
            pc += (x[bi.rs1] == x[bi.rs2]) ? imm_b(bi.imm_0_1_11,bi.imm_12_5_10) : 4;
            return iec::IEC_OK;
        case 1:    //bne
            pc += (x[bi.rs1] != x[bi.rs2]) ? imm_b(bi.imm_0_1_11,bi.imm_12_5_10) : 4;
            return iec::IEC_OK;
        case 4:    //blt
            pc += ((int32_t)x[bi.rs1] < (int32_t)x[bi.rs2]) ? imm_b(bi.imm_0_1_11,bi.imm_12_5_10) : 4;
            return iec::IEC_OK;
        case 5:    //bge
            pc += ((int32_t)x[bi.rs1] >= (int32_t)x[bi.rs2]) ? imm_b(bi.imm_0_1_11,bi.imm_12_5_10) : 4;
            return iec::IEC_OK;
        case 6:    //bltu
            pc += (x[bi.rs1] < x[bi.rs2]) ? imm_b(bi.imm_0_1_11,bi.imm_12_5_10) : 4;
            return iec::IEC_OK;
        case 7:    //blgu
            pc += (x[bi.rs1] >= x[bi.rs2]) ? imm_b(bi.imm_0_1_11,bi.imm_12_5_10) : 4;
            return iec::IEC_OK;
        default:
            // we should never be here
            return iec::Illegal;
    }
}

void RISCVCpu::run_interrupt(bool exeption, uint32_t interrupt_cause) {

    uint32_t m_cause = interrupt_cause | (exeption << 31);
    csr[CSR_MEPC] = pc;
    iescratch = csr[CSR_MIE];
    csr[CSR_MIE] = 0;
    csr[CSR_MSCAUSE] = m_cause;
    pc = csr[CSR_MTVEC];

}

iec RISCVCpu::csr_instr(iType csri) {

    uint32_t _swap;
    auto f3 = (F3_System)csri.func3;
    switch (f3) {
        case F3_System::ECALL:
            if(csri.imm == 1) { pc = 0; return iec::IEC_OK;} //stop program  // EBREAK
            if(csri.imm == 0x302  ) {pc = csr[CSR_MEPC]; csr[CSR_MIE] = iescratch; return iec::IEC_OK;} // MRET
            pc += 4;
            return iec::ECALL_M;

        case F3_System::CSR_RW:
            _swap = csr[csri.imm];
            csr[csri.imm] = x[csri.rs1];
            x[csri.rd] = _swap;
            return iec::IEC_OK;

        case F3_System::CSR_RS:
            _swap = csr[csri.imm];
            csr[csri.imm] |= x[csri.rs1];
            x[csri.rd] = _swap;
            return iec::IEC_OK;

        case F3_System::CSR_RC:
            _swap = csr[csri.imm];
            csr[csri.imm] ^= x[csri.rs1];
            x[csri.rd] = _swap;
            return iec::IEC_OK;

        case F3_System::CSR_RWI:
            _swap = csr[csri.imm];
            csr[csri.imm] = csri.rs1;
            x[csri.rd] = _swap;
            return iec::IEC_OK;

        case F3_System::CSR_RSI:
            _swap = csr[csri.imm];
            csr[csri.imm] |= csri.rs1;
            x[csri.rd] = _swap;
            return iec::IEC_OK;

        case F3_System::CSR_RCI:
            _swap = csr[csri.imm];
            csr[csri.imm] ^= csri.rs1;
            x[csri.rd] = _swap;
            return iec::IEC_OK;

        default:
            // we should never be here
            return iec::Illegal;
    }

}

int RISCVCpu::check_local_interrupt() {

    if ((csr[CSR_MSTATUS] & 8) > 0) { //Status enables interrupts
        uint32_t mip = csr[CSR_MIP];
        uint32_t mie = csr[CSR_MIE];
        mip = (mip & mie);

        switch (mip) {
            case 1 << 3:
                run_interrupt(false, 3);  //Software interrupt
                return 1;
            case 1 << 7:
                run_interrupt(false, 7);  //Timer interrupt
                return 1;
            case 1 << 11:
                run_interrupt(false, 11);  //External interrupt
                return 1;
        }
    }

    return 0;

}

void RISCVCpu::interrupt(bool exp, uint32_t interrupt_cause) {
    if (exp) {
        exception(static_cast<InstructionExceptionCode>(interrupt_cause));
    } else {

        csr[CSR_MIP] |= (1 << interrupt_cause); //set pending bit
        check_local_interrupt(); // does the check for mie filter and executes the interrupt
    }




}