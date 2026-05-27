// Intentionally invalid: direct-slot pool is hidden even from complete façade.
#include <conflux/complete.hxx>

int main() {
	DirectSlotPool pool{1};
	(void)pool;
}
