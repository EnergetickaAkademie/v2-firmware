# Firmware for ENAKv2 boards

Notes: for testing, disconnect WCHLink, or the CH32V003 won't respond to UART commands.

`Serial.begin()` does not work, `if (USART1->STATR & USART_STATR_RXNE) { char incomingChar = USART1->DATAR & 0xFF; }` does. Huh...