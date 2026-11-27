#define U5BLE_NO_MAIN 1
#include "u6hud_ble.c"
int main(void) {
    static struct Diag source;
    iap=&source;
    source.producer_pid=123;
    source.session=1;
    source.phase=RECEIVING;
    source.callback_ready=1;
    source.auth_complete_session=1;
    source.pass_mode=0;
    if(!carplay_ready())return 1;
    source.enabled=0;if(!carplay_ready())return 2;
    source.phase=WAITING;if(carplay_ready())return 3;
    source.phase=SUBMITTED;if(!carplay_ready())return 4;
    source.session=2;if(carplay_ready())return 5;
    source.auth_complete_session=2;if(!carplay_ready())return 6;
    source.callback_ready=0;if(carplay_ready())return 7;
    return 0;
}
