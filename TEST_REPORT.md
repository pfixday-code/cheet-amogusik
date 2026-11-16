# Тест на устройстве — Samsung Galaxy Z Fold4 (SM-F916B), Android 13, Magisk 31.1, Zygisk on

Сборка: Actions run `au_zygisk-magisk` (commit b1ffa98, `au_zygisk-2.3.0.zip`, 305 КБ).
Установка: `magisk --install-module`, перезагрузка, VPN включён, Among Us v19.0.1a (build 7616).

## Что работает ✅

- Модуль грузится в игру: `module loaded in com.innersloth.spacemafia (build Nov 16 2026 20:39:06)`.
- il2cpp-резолвер: все классы/поля найдены на этом билде:
  `PlayerControl.MyPhysics=208, Collider=200, killTimer=176, PlayerPhysics.Speed=80`,
  `PlayerControl.FixedUpdate`, `ShipStatus.CalculateLightRadius` — хуки встали.
- `features: speed=1 noclip=1 light=1 kill=1` — все фичи доступны.
- Java-меню: dex загрузился, FAB «AU» нарисован, панель открывается,
  плашка статуса переключается в `IN GAME` в матче, слайдер Speed сохранил
  значение ×4.00 между заходами в меню.
- Дамп классов: `dump: wrote 3028 classes` — кнопка дампа работает.
- Перехват ввода: `input: wrapped nativeInjectEvent(Landroid/view/InputEvent;I)Z`.

## Что не работает ❌ — геометрия FAB/панели на этом экране

Телефон — складной. Игра рендерится в compat-масштабе (Samsung letterbox):

```
dumpsys activity top:
  mCurrentConfig={0.8 ... sw320dp w855dp h281dp 360dpi ... land
    winConfig={ mBounds=Rect(0,0 - 1994,720) mAppBounds=Rect(68,0 - 1993,686)
    mDisplayRotation=ROTATION_90 }}
display: logicalFrame=[0,0,2658,960] physicalFrame=[0,0,2259,816]
left cutout inset = 92px
```

Симптомы (подтверждены человеком на живом телефоне и uiautomator-дампами):

1. **Нарисованная кнопка AU не совпадает со своей touch-зоной.**
   Кнопка видна на физическом экране в точке ≈(170, 396) px, а её
   доступная (accessibility) зона — `bounds=[115,288][237,410]`.
   Расхождение по вертикали ≈ 100–120 логических px. Тап по нарисованной
   кнопке попадает «мимо»; тап в зону выше — ловит кнопку.
2. **FAB перетаскивается только в узкой области** — drag живёт внутри
   перелэйаутнутой области, а не по всему видимому экрану.
3. **Панель открывается стабильно только после того, как вылезет клавиатура** —
   IME вызывает relayout окна, после этого координаты совпадают и всё работает.
   До первого relayout — рисование и тач разъехались.

Версия панели, когда открылась: «Among Us · Zygisk menu · v2.3», статус IN GAME,
разделы MOVEMENT (Speed, Noclip), VISION — всё отрисовано, слайдер ×4.00.

## Доп. замечания

- Игра рендерит через **Vulkan** (`libvulkan.so mapped`, `eglSwapBuffers` ни разу
  не вызвался) — GLES ImGui-оверлей на этом устройстве не рисует, лог сам это
  честно пишет. Не блокер: основное меню — Java.
- Хук `CalculateLightRadius` сработал в матче: `first call, radius 5.000`.

## Что нужно от тебя

Фикс рассинхрона координат FAB/панели в `Menu.java` для экрана с
compat-масштабом (density 0.8, app bounds 1994x720 при физических 2658x960):
судя по симптомам, позиция задаётся в одном пространстве, а тач/лейаут живут
в другом (или лейаут не перерасчитывается до первого resize окна).
Готов забрать обновлённые `java/au/menu/Menu.java` (+ что ещё скажешь),
закоммичу, Actions пересоберёт, поставлю и перепроверю. Логи/дампы приложу
по запросу.

— отчёт собрал агент с ПК, тест живой, устройство в строю.
