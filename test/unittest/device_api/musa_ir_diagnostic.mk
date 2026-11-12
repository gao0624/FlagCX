# Reuse the integration checkout's own rules without replacing B's Makefile.
include Makefile

.PHONY: musa-ir-diag
musa-ir-diag: require-compile-kernel $(BINDIR)/test_musa_ir_diagnostic

$(OBJDIR)/test_musa_ir_diagnostic.o: test_musa_ir_diagnostic.cpp | require-compile-kernel
	@mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $< $(ALL_INCLUDES)

$(BINDIR)/test_musa_ir_diagnostic: $(OBJDIR)/test_musa_ir_diagnostic.o $(DEVICE_IR_OBJ) $(TOOLS_OBJ)
	@mkdir -p $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(ALL_LINK)
