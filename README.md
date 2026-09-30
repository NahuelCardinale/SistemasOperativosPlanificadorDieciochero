# Tarea 1: Planificador Dieciochero - Sistemas Operativos

## Integrante
- Nahuel Sánchez

## Uso de IA y Autoría
Por la falta de tiempo antes de la hora de entrega, usé Inteligencia Artificial para que me ayudara a armar el código final. Varias de las ideas de diseño más complejas (como la forma de manejar la memoria dinámica o cómo detener los procesos de forma segura) fueron sugeridas por la IA. 

A pesar de esto, entiendo perfectamente cómo funciona cada línea de código, las funciones de POSIX que se usaron y la lógica detrás del programa. El trabajo demuestra que sé cómo resolver la problemática usando memoria dinámica, procesos y tuberías (pipes), cumpliendo estrictamente con la regla de no usar hilos ni herramientas de sincronización prohibidas.

## Compilación y Ejecución
El programa está escrito en estándar C17. Para compilarlo usando las reglas estrictas que exige la pauta, usa este comando:
```bash
gcc -Wall -Wextra -std=c17 -lpthread planificador.c -o planificador