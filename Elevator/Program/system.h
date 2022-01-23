#include <iostream>
#include <cstdlib>
#include <ctime>

#define STACK_INIT_SIZE 5
#define STACK_INCREMENT 5

#define OVERFLOW -2
#define STACK_EMPTY -3
#define QUEUE_EMPTY -4
#define UNKNOWN_ERROR -5

enum ShowType
{
    NEW_PASSENGER,
    GIVE_UP,
    PASSENGER_OUT,
    PASSENGER_IN,
    DOOR_CLOSING,
    DOOR_REOPENING,
    ELEVATOR_ACCELERATING,
    BE_IDLE,
    IS_MOVING,
    IS_SLOWING_DOWN,
    ARRIVE_AND_OPENING,
    IDLE_RETURN,
    IDLE_OPENING
};

enum ElevatorTime
{
    DOOR_TIME = 20,
    CLOSING_TEST_TIME = 40,
    IN_OUT_TIME = 25,
    MAX_WAITING_TIME = 300,
    ACCELERATE_TIME = 15,
    UP_TIME = 51,
    DOWN_TIME = 61,
    UP_SLOW_TIME = 14,
    DOWN_SLOW_TIME = 23
};

enum PassengerTime
{
    MAX_GIVE_UP_TIME = 1801,
    MAX_INTER_TIME = 551
};

enum ElevatorState
{
    GOING_UP,
    GOING_DOWN,
    IDLE
};

enum ElevatorMove
{
    OPENING,
    OPENED,
    CLOSING,
    CLOSED,
    ACCELERATING,
    MOVING,
    SLOWING_DOWN,
    WAITING
};

enum WaitQueueType
{
    UP,
    DOWN
}; // use g_wait_queue[UP] instead of g_wait_queue[0]

typedef struct Passenger
{
    int id;
    int in_floor;
    int out_floor;
    int give_up_time;
} Passenger, *PassengerPtr;

typedef struct PassengerStack
{
    PassengerPtr *base;
    PassengerPtr *top;
    int stack_size;
} PassengerStack, *PassengerStackPtr;

typedef struct WaitQueueNode
{
    PassengerPtr data;
    struct WaitQueueNode *next;
} WaitQueueNode, *WaitQueueNodePtr;

typedef struct WaitQueue
{
    WaitQueueNodePtr front;
    WaitQueueNodePtr rear;
    int num;
} WaitQueue, *WaitQueuePtr;

typedef struct Elevator
{
    // about stack
    int passenger_num;
    int capacity;
    PassengerStackPtr stack_array; // each floor has a stack for each lift

    // about who is in the elevator
    int *passenger_id;

    // about button
    // simplify:
    // each elevator has its own call_car button, but they share the same call_up and call_down button
    int *call_car;

    // about floor
    int current_floor; // 0 is the lowest floor
    int total_floor;
    int idle_floor;

    // about state and move
    ElevatorMove move;
    ElevatorState state;
    int move_timer;   // timer for current move, countdown
    int in_out_timer; // timer for in-out event, countdown
    int whether_change_state;
} Elevator, *ElevatorPtr;

void Show(int type, int id, int in_floor, int out_floor, int lift, int lift_floor);