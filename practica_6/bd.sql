-- Script para crear la base de datos y tabla de usuarios
-- para la práctica 3 de redes

-- Crear la base de datos
CREATE DATABASE IF NOT EXISTS practica3;
USE practica3;

-- Crear tabla de usuarios
CREATE TABLE IF NOT EXISTS usuarios (
    id INT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(50) NOT NULL UNIQUE,
    password VARCHAR(100) NOT NULL,
    fecha_creacion TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Insertar algunos usuarios de prueba
INSERT INTO usuarios (username, password) VALUES 
('admin', 'password'),
('rodrigo', '123456'),
('luis', 'mipassword'),
('usuario1', 'test123'),
('guest', 'guest123');

-- Mostrar los usuarios creados
SELECT id, username, fecha_creacion FROM usuarios;



-- Crear tabla de usuarios
CREATE TABLE IF NOT EXISTS usuarios (
    id INT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(50) NOT NULL UNIQUE,
    password VARCHAR(100) NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Crear tabla de mensajes
CREATE TABLE IF NOT EXISTS mensajes (
    id INT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(50) NOT NULL,
    mensaje TEXT NOT NULL,
    timestamp DATETIME NOT NULL,
    editado BOOLEAN DEFAULT FALSE,
    timestamp_edicion DATETIME NULL,
    FOREIGN KEY (username) REFERENCES usuarios(username)
);

-- Crear usuario para la aplicación
CREATE USER IF NOT EXISTS 'practica3_user'@'localhost' IDENTIFIED BY 'tupassword';


--PARA 50 CLIENTES
-- Insertar 50 usuarios de prueba con fecha de creación
DELETE FROM mensajes;
DELETE FROM usuarios;

INSERT INTO usuarios (username, password, fecha_creacion) VALUES
('usuario1', 'password1', NOW()),
('usuario2', 'password2', NOW()),
('usuario3', 'password3', NOW()),
('usuario4', 'password4', NOW()),
('usuario5', 'password5', NOW()),
('usuario6', 'password6', NOW()),
('usuario7', 'password7', NOW()),
('usuario8', 'password8', NOW()),
('usuario9', 'password9', NOW()),
('usuario10', 'password10', NOW()),
('usuario11', 'password11', NOW()),
('usuario12', 'password12', NOW()),
('usuario13', 'password13', NOW()),
('usuario14', 'password14', NOW()),
('usuario15', 'password15', NOW()),
('usuario16', 'password16', NOW()),
('usuario17', 'password17', NOW()),
('usuario18', 'password18', NOW()),
('usuario19', 'password19', NOW()),
('usuario20', 'password20', NOW()),
('usuario21', 'password21', NOW()),
('usuario22', 'password22', NOW()),
('usuario23', 'password23', NOW()),
('usuario24', 'password24', NOW()),
('usuario25', 'password25', NOW()),
('usuario26', 'password26', NOW()),
('usuario27', 'password27', NOW()),
('usuario28', 'password28', NOW()),
('usuario29', 'password29', NOW()),
('usuario30', 'password30', NOW()),
('usuario31', 'password31', NOW()),
('usuario32', 'password32', NOW()),
('usuario33', 'password33', NOW()),
('usuario34', 'password34', NOW()),
('usuario35', 'password35', NOW()),
('usuario36', 'password36', NOW()),
('usuario37', 'password37', NOW()),
('usuario38', 'password38', NOW()),
('usuario39', 'password39', NOW()),
('usuario40', 'password40', NOW()),
('usuario41', 'password41', NOW()),
('usuario42', 'password42', NOW()),
('usuario43', 'password43', NOW()),
('usuario44', 'password44', NOW()),
('usuario45', 'password45', NOW()),
('usuario46', 'password46', NOW()),
('usuario47', 'password47', NOW()),
('usuario48', 'password48', NOW()),
('usuario49', 'password49', NOW()),
('usuario50', 'password50', NOW());

-- Verificar la inserción
SELECT COUNT(*) as total_usuarios FROM usuarios;

CREATE TABLE key_value_store (
    id INT AUTO_INCREMENT PRIMARY KEY,
    clave VARCHAR(255) NOT NULL UNIQUE,
    valor TEXT,
    es_binario TINYINT(1) DEFAULT 0,
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
