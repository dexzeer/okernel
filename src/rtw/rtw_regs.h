#ifndef RTW_REGS_H
#define RTW_REGS_H

// Registers and bits of the Realtek RTL8821C family (8821CU on USB, 8821CE on PCIe) used by src/rtw. The facts come from
// the public rtw88 driver's register definitions; the names are descriptive.
#define B(n) (1u << (n))

#define R_SYS_FUNC_EN       0x0002
#define R_APS_FSMCO         0x0004      // power state machine
#define R_SYS_CLK_CTRL      0x0008
#define R_RSV_CTRL          0x001C
#define R_RF_CTRL           0x001F
#define R_AFE_XTAL_CTRL     0x0024
#define R_AFE_PLL_CTRL      0x0028
#define R_EFUSE_CTRL        0x0030
#define R_LDO_EFUSE_CTRL    0x0034
#define R_GPIO_MUXCFG       0x0040
#define R_LED_CFG           0x004C
#define R_PAD_CTRL1         0x0064
#define R_HCI_OPT_CTRL      0x0074
#define R_MCUFW_CTRL        0x0080
#define R_EFUSE_ACCESS      0x00CF
#define R_WLRF1             0x00EC
#define R_SYS_CFG1          0x00F0
#define R_SYS_STATUS1       0x00F4
#define R_CR                0x0100
#define R_TXDMA_PQ_MAP      0x010C
#define R_RXFF_BNDY         0x011C
#define R_C2HEVT            0x01A0
#define R_HMETFR            0x01CC
#define R_HMEBOX0           0x01D0
#define R_HMEBOX0_EX        0x01F0
#define R_FIFOPAGE_CTRL_2   0x0204
#define R_AUTO_LLT_V1       0x0208
#define R_TXDMA_OFFSET_CHK  0x020C
#define R_TXDMA_STATUS      0x0210
#define R_RQPN_CTRL_2       0x022C
#define R_FIFOPAGE_INFO_1   0x0230
#define R_FIFOPAGE_INFO_2   0x0234
#define R_FIFOPAGE_INFO_3   0x0238
#define R_FIFOPAGE_INFO_4   0x023C
#define R_FIFOPAGE_INFO_5   0x0240
#define R_H2C_HEAD          0x0244
#define R_H2C_TAIL          0x0248
#define R_H2C_READ_ADDR     0x024C
#define R_H2C_INFO          0x0254
#define R_RXDMA_AGG_PG_TH   0x0280
#define R_RXDMA_MODE        0x0290
#define R_FWHW_TXQ_CTRL     0x0420
#define R_HWSEQ_CTRL        0x0423
#define R_BCNQ_BDNY_V1      0x0424
#define R_BCNQ1_BDNY_V1     0x0456
#define R_RRSR              0x0440
#define R_DATA_SC           0x0483
#define R_INIRTS_RATE_SEL   0x0480
#define R_PROT_MODE_CTRL    0x04C8
#define R_BAR_MODE_CTRL     0x04CC
#define R_PRECNT_CTRL       0x04E5
#define R_TX_HANG_CTRL      0x045E
#define R_AMPDU_MAX_TIME_V1 0x0455
#define R_FAST_EDCA_CTRL    0x0460
#define R_QUEUE_CTRL        0x04C6
#define R_EDCA_VO_PARAM     0x0500
#define R_EDCA_VI_PARAM     0x0504
#define R_BCNTCFG           0x0510
#define R_PIFS              0x0512
#define R_SIFS              0x0514
#define R_SLOT              0x051B
#define R_TX_PTCL_CTRL      0x0520
#define R_TXPAUSE           0x0522
#define R_TBTT_PROHIBIT     0x0540
#define R_RD_NAV_NXT        0x0544
#define R_BCN_CTRL          0x0550
#define R_DRVERLYINT        0x0558
#define R_BCNDMATIM         0x0559
#define R_USTIME_TSF        0x055C
#define R_RXTSF_OFFSET_CCK  0x055E
#define R_TIMER0_SRC_SEL    0x05B4
#define R_MACID             0x0610
#define R_BSSID             0x0618
#define R_TCR               0x0604
#define R_RCR               0x0608
#define R_RX_PKT_LIMIT      0x060C
#define R_RX_DRVINFO_SZ     0x060F
#define R_MAR               0x0620
#define R_USTIME_EDCA       0x0638
#define R_ACKTO_CCK         0x0639
#define R_WMAC_TRXPTCL_CTL  0x0668
#define R_WMAC_TRXPTCL_CTL_H 0x066C
#define R_RXFLTMAP0         0x06A0
#define R_RXFLTMAP1         0x06A2
#define R_RXFLTMAP2         0x06A4
#define R_WMAC_OPTION_FUNCTION 0x07D0
#define R_SND_PTCL_CTRL     0x0718
#define R_CCK_CHECK         0x0454
#define R_FAST_EDCA_VOVI    0x1448
#define R_FAST_EDCA_BEBK    0x144C
#define R_CPU_DMEM_CON      0x1080
#define R_CR_EXT            0x1100
#define R_DDMA_CH0SA        0x1200
#define R_DDMA_CH0DA        0x1204
#define R_DDMA_CH0CTRL      0x1208
#define R_H2CQ_CSR          0x1330
#define R_H2C_PKT_READADDR  0x10D0
#define R_H2C_PKT_WRITEADDR 0x10D4
#define R_FW_DBG7           0x10FC
#define R_RFE_CTRL          0x0CB8
#define R_RXPSEL            0x0808
#define R_AFE_CTRL1         0x0024
#define R_REG_SEC           0x04E0      // written after accesses to the always-on register section (USB)

// R_CR
#define CR_HCI_TXDMA_EN B(0)
#define CR_HCI_RXDMA_EN B(1)
#define CR_TXDMA_EN     B(2)
#define CR_RXDMA_EN     B(3)
#define CR_PROTOCOL_EN  B(4)
#define CR_SCHEDULE_EN  B(5)
#define CR_MACTXEN      B(6)
#define CR_MACRXEN      B(7)
#define CR_ENSWBCN      B(8)
#define MAC_TRX_ENABLE  (CR_HCI_TXDMA_EN | CR_HCI_RXDMA_EN | CR_TXDMA_EN | CR_RXDMA_EN | CR_PROTOCOL_EN | CR_SCHEDULE_EN | CR_MACTXEN | CR_MACRXEN)

// R_MCUFW_CTRL
#define MCUFW_EN        B(0)
#define MCUFW_RDY       B(1)
#define FW_DW_RDY       B(14)
#define FW_INIT_RDY     B(15)
#define IMEM_DW_OK      B(3)
#define DMEM_DW_OK      B(5)
#define CHECK_SUM_OK    (B(4) | B(6))
#define CPU_CLK_SEL     (B(12) | B(13))
#define FW_READY        (FW_INIT_RDY | FW_DW_RDY | IMEM_DW_OK | DMEM_DW_OK | CHECK_SUM_OK)
#define FW_READY_MASK   (0xffffu & ~CPU_CLK_SEL)

// DDMA channel 0
#define DDMA_OWN        B(31)
#define DDMA_CHKSUM_EN  B(29)
#define DDMA_CHKSUM_STS B(27)
#define DDMA_RESET_CHKSUM_STS B(25)
#define DDMA_CHKSUM_CONT B(24)
#define DDMA_DLEN_MASK  0x3ffff
#define OCPBASE_TXBUF   0x18780000u
#define OCPBASE_DMEM    0x00200000u

// R_CPU_DMEM_CON
#define WL_PLATFORM_RST B(16)
#define DDMA_EN         B(8)

// R_BCN_CTRL
#define DIS_TSF_UDT     B(4)
#define EN_BCN_FUNCTION B(3)

// R_RCR bits
#define RCR_APP_PHYSTS  B(28)
#define RCR_AAP         B(0)   // accept all unicast
#define RCR_APM         B(1)   // accept physical match
#define RCR_AM          B(2)   // accept multicast
#define RCR_AB          B(3)   // accept broadcast
#define RCR_ACRC32      B(8)   // accept frames with a bad CRC

#endif
